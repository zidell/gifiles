#include "Pty.h"

#include <QDebug>
#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QProcessEnvironment>

#include <atomic>
#include <thread>

#include <windows.h>
#include <tlhelp32.h>

struct Pty::Impl {
    HPCON console = nullptr;
    HANDLE input = INVALID_HANDLE_VALUE;  // we write keystrokes here
    HANDLE output = INVALID_HANDLE_VALUE; // the console's output arrives here
    PROCESS_INFORMATION pi = {};
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    std::thread reader;
    std::atomic<bool> running{false};
};

Pty::Pty(QObject *parent) : QObject(parent), d(std::make_unique<Impl>()) {}

Pty::~Pty()
{
    d->running = false;
    if (d->pi.hProcess) {
        TerminateProcess(d->pi.hProcess, 0);
        CloseHandle(d->pi.hProcess);
        CloseHandle(d->pi.hThread);
    }
    if (d->console)
        ClosePseudoConsole(d->console); // also ends the reader's ReadFile
    if (d->reader.joinable())
        d->reader.join();
    if (d->input != INVALID_HANDLE_VALUE)
        CloseHandle(d->input);
    if (d->output != INVALID_HANDLE_VALUE)
        CloseHandle(d->output);
    if (d->attrs) {
        DeleteProcThreadAttributeList(d->attrs);
        HeapFree(GetProcessHeap(), 0, d->attrs);
    }
}

static QString quoteArg(const QString &a)
{
    if (!a.isEmpty() && !a.contains(QLatin1Char(' ')) && !a.contains(QLatin1Char('"')))
        return a;
    QString q = a;
    q.replace(QLatin1String("\""), QLatin1String("\\\""));
    return QLatin1Char('"') + q + QLatin1Char('"');
}

bool Pty::start(const QString &program, const QStringList &args, const QString &, const QString &cwd,
                int cols, int rows, const QStringList &extraEnv)
{
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, nullptr, 0) || !CreatePipe(&outRead, &outWrite, nullptr, 0)) {
        qWarning("Pty: CreatePipe failed (%lu)", GetLastError());
        return false;
    }
    const COORD size{SHORT(cols), SHORT(rows)};
    if (const HRESULT hr = CreatePseudoConsole(size, inRead, outWrite, 0, &d->console); FAILED(hr)) {
        qWarning("Pty: CreatePseudoConsole failed (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    CloseHandle(inRead); // now owned by the pseudo console
    CloseHandle(outWrite);
    d->input = inWrite;
    d->output = outRead;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    d->attrs = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!InitializeProcThreadAttributeList(d->attrs, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(d->attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, d->console, sizeof(HPCON),
                                   nullptr, nullptr)) {
        qWarning("Pty: console attribute failed (%lu)", GetLastError());
        return false;
    }

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si.lpAttributeList = d->attrs;
    // Without this, a child of a process whose std handles are redirected (started from a terminal,
    // CI) inherits those instead of the pseudo console, reads EOF on stdin and exits at once.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    QString cmd = quoteArg(program);
    for (const QString &a : args)
        cmd += QLatin1Char(' ') + quoteArg(a);
    std::wstring cmdLine = cmd.toStdWString();
    const std::wstring dir = QDir::toNativeSeparators(cwd).toStdWString();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &kv : extraEnv) {
        const int eq = kv.indexOf(QLatin1Char('='));
        env.insert(kv.left(eq), kv.mid(eq + 1));
    }
    std::wstring block;
    for (const QString &kv : env.toStringList())
        block += kv.toStdWString() + L'\0';
    block += L'\0';

    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, block.data(),
                        dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &d->pi)) {
        qWarning("Pty: CreateProcess failed for %ls (%lu)", cmdLine.c_str(), GetLastError());
        return false;
    }

    d->running = true;
    QPointer<Pty> self(this);
    HANDLE out = d->output;
    HANDLE proc = d->pi.hProcess;
    d->reader = std::thread([self, out, proc, this] {
        char buf[16384];
        DWORD n = 0;
        while (d->running && ReadFile(out, buf, sizeof buf, &n, nullptr) && n > 0) {
            QByteArray chunk(buf, int(n));
            QMetaObject::invokeMethod(self, [self, chunk] { if (self) emit self->dataReceived(chunk); }, Qt::QueuedConnection);
        }
        Q_UNUSED(proc);
        QMetaObject::invokeMethod(self, [self] {
            if (self) {
                self->d->running = false;
                emit self->finished();
            }
        }, Qt::QueuedConnection);
    });
    // The pipe stays open after the shell exits while the console lives; watch the process too.
    std::thread([self, proc] {
        WaitForSingleObject(proc, INFINITE);
        QMetaObject::invokeMethod(self, [self] {
            if (self && self->d->console) {
                ClosePseudoConsole(self->d->console); // unblocks the reader, which emits finished()
                self->d->console = nullptr;
            }
        }, Qt::QueuedConnection);
    }).detach();
    return true;
}

bool Pty::isRunning() const
{
    return d->running;
}

void Pty::write(const QByteArray &data)
{
    DWORD written = 0;
    const char *p = data.constData();
    DWORD left = DWORD(data.size());
    while (left > 0 && WriteFile(d->input, p, left, &written, nullptr) && written > 0) {
        p += written;
        left -= written;
    }
}

void Pty::resize(int cols, int rows)
{
    if (d->console)
        ResizePseudoConsole(d->console, COORD{SHORT(cols), SHORT(rows)});
}

bool Pty::isShellIdle() const
{
    // Idle when the shell has no child processes.
    if (!d->running)
        return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return true;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof pe;
    bool idle = true;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ParentProcessID == d->pi.dwProcessId && std::wstring(pe.szExeFile) != L"conhost.exe") {
            idle = false;
            break;
        }
    CloseHandle(snap);
    return idle;
}

QString Pty::shellCwd() const
{
    return {}; // PowerShell reports its location through OSC 7 (see TerminalWidget)
}
