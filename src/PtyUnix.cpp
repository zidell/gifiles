#include "Pty.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(Q_OS_MACOS)
#include <libproc.h>
#include <util.h>
#else
#include <pty.h>
#endif

struct Pty::Impl {
    int master = -1;
    pid_t pid = -1;
    QSocketNotifier *readNotifier = nullptr;
    QSocketNotifier *writeNotifier = nullptr;
    QByteArray pendingWrite;
};

// Reaps the child without blocking the GUI: macOS can keep an exiting shell in the kernel for half a
// minute (a zombie waiting on its tty while a program like a TUI agent still holds it), and a blocking
// waitpid() on the GUI thread froze the whole app when ⌘W closed such a tab.
static void reap(pid_t pid)
{
    int status;
    if (::waitpid(pid, &status, WNOHANG) != 0)
        return;
    std::thread([pid] {
        int st;
        ::waitpid(pid, &st, 0);
    }).detach();
}

Pty::Pty(QObject *parent) : QObject(parent), d(std::make_unique<Impl>()) {}

Pty::~Pty()
{
    // Close the master first: a dying shell (or its tty) can wait to drain output nobody reads
    // any more, and waitpid() below would wait forever.
    delete d->readNotifier;
    delete d->writeNotifier;
    if (d->master >= 0)
        ::close(d->master);
    if (d->pid > 0) {
        ::kill(d->pid, SIGHUP);
        int status;
        if (::waitpid(d->pid, &status, WNOHANG) == 0) {
            ::usleep(20000);
            if (::waitpid(d->pid, &status, WNOHANG) == 0) {
                ::kill(d->pid, SIGKILL);
                reap(d->pid);
            }
        }
    }
}

bool Pty::start(const QString &program, const QStringList &args, const QString &argv0, const QString &cwd,
                int cols, int rows, const QStringList &extraEnv)
{
    // Everything the child needs is prepared before fork(): after it only exec-safe calls run.
    const QByteArray prog = QFile::encodeName(program);
    std::vector<QByteArray> argvStore;
    argvStore.push_back(argv0.isEmpty() ? prog : argv0.toLocal8Bit());
    for (const QString &a : args)
        argvStore.push_back(a.toLocal8Bit());
    std::vector<char *> argv;
    for (QByteArray &a : argvStore)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &kv : extraEnv) {
        const int eq = kv.indexOf(QLatin1Char('='));
        env.insert(kv.left(eq), kv.mid(eq + 1));
    }
    std::vector<QByteArray> envStore;
    for (const QString &kv : env.toStringList())
        envStore.push_back(kv.toLocal8Bit());
    std::vector<char *> envp;
    for (QByteArray &e : envStore)
        envp.push_back(e.data());
    envp.push_back(nullptr);
    const QByteArray dir = QFile::encodeName(cwd);

    struct winsize ws = {};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0)
        return false;
    if (pid == 0) {
        if (!dir.isEmpty())
            (void)::chdir(dir.constData());
        ::execve(prog.constData(), argv.data(), envp.data());
        ::_exit(127);
    }
    d->pid = pid;
    d->master = master;
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);

    d->readNotifier = new QSocketNotifier(master, QSocketNotifier::Read, this);
    connect(d->readNotifier, &QSocketNotifier::activated, this, [this] {
        char buf[16384];
        QByteArray out;
        for (;;) {
            const ssize_t n = ::read(d->master, buf, sizeof buf);
            if (n > 0) {
                out.append(buf, n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                break;
            // EOF or EIO: the shell has exited.
            d->readNotifier->setEnabled(false);
            if (!out.isEmpty())
                emit dataReceived(out);
            reap(d->pid);
            d->pid = -1;
            emit finished();
            return;
        }
        if (!out.isEmpty())
            emit dataReceived(out);
    });
    d->writeNotifier = new QSocketNotifier(master, QSocketNotifier::Write, this);
    d->writeNotifier->setEnabled(false);
    connect(d->writeNotifier, &QSocketNotifier::activated, this, [this] { write({}); });
    return true;
}

bool Pty::isRunning() const
{
    return d->pid > 0;
}

void Pty::write(const QByteArray &data)
{
    if (d->master < 0)
        return;
    d->pendingWrite += data;
    while (!d->pendingWrite.isEmpty()) {
        const ssize_t n = ::write(d->master, d->pendingWrite.constData(), size_t(d->pendingWrite.size()));
        if (n > 0) {
            d->pendingWrite.remove(0, n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break; // EAGAIN: wait until writable
    }
    d->writeNotifier->setEnabled(!d->pendingWrite.isEmpty());
}

void Pty::resize(int cols, int rows)
{
    if (d->master < 0)
        return;
    struct winsize ws = {};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ::ioctl(d->master, TIOCSWINSZ, &ws);
}

bool Pty::isShellIdle() const
{
    return d->master >= 0 && d->pid > 0 && ::tcgetpgrp(d->master) == d->pid;
}

QString Pty::shellCwd() const
{
    if (d->pid <= 0)
        return {};
#if defined(Q_OS_MACOS)
    struct proc_vnodepathinfo vpi;
    if (::proc_pidinfo(d->pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) == int(sizeof vpi))
        return QFile::decodeName(vpi.pvi_cdir.vip_path);
    return {};
#else
    return QFileInfo(QStringLiteral("/proc/%1/cwd").arg(d->pid)).symLinkTarget();
#endif
}
