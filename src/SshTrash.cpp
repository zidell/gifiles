#include "SshTrash.h"
#include "Util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <cstring>
#include <vector>
#include <QJsonObject>
#include <QProcess>
#include <QUuid>
#include <map>

#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#endif

namespace SshTrash {
namespace {
const QString prefix = QStringLiteral("gifiles-ssh-trash:");
const QByteArray marker("GIFILES_TRASH ");

QString shellQuote(QString text)
{
    return QLatin1Char('\'') + text.replace(QLatin1Char('\''), QLatin1String("'\\''")) + QLatin1Char('\'');
}

QString endpoint(const Mount &m)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("host"), m.host},
        {QStringLiteral("options"), QJsonArray::fromStringList(m.options)}}).toJson(QJsonDocument::Compact));
}
} // namespace

std::optional<Mount> parseMount(const QStringList &argv)
{
    if (argv.isEmpty() || QFileInfo(argv.first()).fileName() != QLatin1String("sshfs"))
        return std::nullopt;
    Mount m;
    QStringList positional;
    int port = 0;
    auto option = [&m, &port](const QString &value) {
        for (const QString &part : value.split(QLatin1Char(','))) {
            const QString key = part.section(QLatin1Char('='), 0, 0);
            // Preserve SSH connection parameters, not filesystem options or a custom command.
            static const QStringList allowed{QStringLiteral("IdentityFile"), QStringLiteral("IdentitiesOnly"),
                QStringLiteral("ProxyJump"), QStringLiteral("UserKnownHostsFile"), QStringLiteral("HostKeyAlias"),
                QStringLiteral("HostName"), QStringLiteral("User")};
            if (key.compare(QLatin1String("port"), Qt::CaseInsensitive) == 0) {
                bool ok = false;
                const int next = part.section(QLatin1Char('='), 1).toInt(&ok);
                if (!ok || next < 1 || next > 65535 || (port && port != next))
                    m.error = part;
                else
                    port = next;
            } else if (key.compare(QLatin1String("ssh_command"), Qt::CaseInsensitive) == 0 ||
                       key.compare(QLatin1String("ProxyCommand"), Qt::CaseInsensitive) == 0)
                m.error = part;
            else if (allowed.contains(key, Qt::CaseInsensitive))
                m.options << QStringLiteral("-o") << part;
        }
    };
    for (qsizetype i = 1; i < argv.size(); ++i) {
        const QString &arg = argv[i];
        if (arg == QLatin1String("-o") && i + 1 < argv.size())
            option(argv[++i]);
        else if (arg.startsWith(QLatin1String("-o")))
            option(arg.mid(2));
        else if (arg == QLatin1String("-p") && i + 1 < argv.size())
            option(QStringLiteral("port=") + argv[++i]);
        else if (arg.startsWith(QLatin1String("-p")) && arg.size() > 2)
            option(QStringLiteral("port=") + arg.mid(2));
        else if (arg == QLatin1String("-F") && i + 1 < argv.size()) {
            if (!QDir::isAbsolutePath(argv[i + 1]))
                m.error = argv[i + 1];
            m.options << arg << argv[i + 1];
            ++i;
        } else if (arg.startsWith(QLatin1String("-F")) && arg.size() > 2) {
            if (!QDir::isAbsolutePath(arg.mid(2)))
                m.error = arg.mid(2);
            m.options << QStringLiteral("-F") << arg.mid(2);
        }
        else if (!arg.startsWith(QLatin1Char('-')))
            positional << arg;
    }
    if (positional.size() != 2 || !QDir::isAbsolutePath(positional[1]))
        return std::nullopt;
    const QString source = positional[0];
    const qsizetype close = source.indexOf(QLatin1Char(']'));
    const qsizetype colon = close >= 0 ? source.indexOf(QLatin1Char(':'), close) : source.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return std::nullopt;
    m.host = source.left(colon);
    if (m.host.startsWith(QLatin1Char('-')) || m.host.contains(QLatin1Char('\n')) || m.host.contains(QLatin1Char('\r')))
        return std::nullopt;
    if (port)
        m.options << QStringLiteral("-p") << QString::number(port);
    m.localRoot = QDir::cleanPath(positional[1]);
    m.remoteRoot = source.mid(colon + 1);
    return m;
}

QList<Mount> mountedSshfs()
{
    QList<Mount> out;
#ifdef Q_OS_MACOS
    // fuse-t's cached mount source only says "fuse-t:/volume". Read the running sshfs argv
    // without parsing ps output (remote roots/mount points may contain spaces).
    auto discoverProcesses = [&] {
        const int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
        if (bytes <= 0)
            return;
        std::vector<pid_t> pids(size_t(bytes / sizeof(pid_t) + 64));
        const int count = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), int(pids.size() * sizeof(pid_t))) / int(sizeof(pid_t));
        int argMax = 0;
        size_t size = sizeof(argMax);
        int argMib[] = {CTL_KERN, KERN_ARGMAX};
        if (sysctl(argMib, 2, &argMax, &size, nullptr, 0) != 0 || argMax <= 0)
            return;
        for (int i = 0; i < count; ++i) {
            char executable[PROC_PIDPATHINFO_MAXSIZE] = {};
            if (proc_pidpath(pids[i], executable, sizeof(executable)) <= 0 ||
                QFileInfo(QString::fromUtf8(executable)).fileName() != QLatin1String("sshfs"))
                continue;
            QByteArray buffer(argMax, '\0');
            size_t length = size_t(buffer.size());
            int mib[] = {CTL_KERN, KERN_PROCARGS2, pids[i]};
            if (sysctl(mib, 3, buffer.data(), &length, nullptr, 0) != 0 || length <= sizeof(int))
                continue;
            int argc = 0;
            memcpy(&argc, buffer.constData(), sizeof(argc));
            if (argc < 1 || argc > argMax / 2)
                continue;
            const char *p = buffer.constData() + sizeof(argc), *end = buffer.constData() + length;
            while (p < end && *p) ++p; // executable path
            while (p < end && !*p) ++p;
            QStringList argv;
            for (int a = 0; a < argc && p < end; ++a) {
                const char *start = p;
                while (p < end && *p) ++p;
                argv << QString::fromUtf8(start, p - start);
                if (p < end) ++p;
            }
            if (argv.size() != argc)
                continue;
            if (auto mount = parseMount(argv)) {
                bool active = false;
                for (const auto &volume : util::userVolumes())
                    active |= volume.root == mount->localRoot;
                if (active)
                    out << *mount;
            }
        }
    };
    discoverProcesses();
    // Even if argv is inaccessible/unrecognized, a known SSHFS/fuse-t mount must never
    // silently fall through to a client's trash implementation.
    const int mountCount = getfsstat(nullptr, 0, MNT_NOWAIT);
    if (mountCount >= 0) {
        std::vector<struct statfs> mounts(size_t(mountCount + 8));
        const int n = getfsstat(mounts.data(), int(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
        for (int i = 0; i < n; ++i) {
            const QString source = QString::fromUtf8(mounts[i].f_mntfromname);
            const QString type = QString::fromUtf8(mounts[i].f_fstypename);
            const QString root = QString::fromUtf8(mounts[i].f_mntonname);
            if (!source.startsWith(QLatin1String("fuse-t:")) && !type.contains(QLatin1String("sshfs")) &&
                !(type.contains(QLatin1String("fuse")) && source.contains(QLatin1Char(':'))))
                continue;
            bool found = false;
            for (const auto &mount : out)
                found |= mount.localRoot == root;
            if (!found)
                out << Mount{root, {}, {}, {}, QStringLiteral("unresolved SSHFS mount")};
        }
    }
#endif
    return out;
}

QString helperSource()
{
    return QString::fromUtf8(R"SWIFT(import Foundation
let fm = FileManager.default
func exists(_ path: String) -> Bool { (try? fm.attributesOfItem(atPath: path)) != nil }
while let line = readLine() {
    var response: [String: Any] = ["v": 1]
    do {
        let request = try JSONSerialization.jsonObject(with: Data(line.utf8)) as! [String: Any]
        response["id"] = request["id"]
        let action = request["action"] as! String
        if action == "trash" {
            let raw = request["path"] as! String
            let expanded = raw == "~" || raw.hasPrefix("~/") ? (raw as NSString).expandingTildeInPath : raw
            let path = expanded.hasPrefix("/") ? expanded : (NSHomeDirectory() as NSString).appendingPathComponent(expanded)
            let original = URL(fileURLWithPath: path).standardizedFileURL
            var result: NSURL?
            try fm.trashItem(at: original, resultingItemURL: &result)
            guard let trash = result as URL?, !exists(original.path) else {
                throw NSError(domain: "GifilesTrash", code: 1, userInfo: [NSLocalizedDescriptionKey: "Trash operation was not confirmed"])
            }
            response["original"] = original.path
            response["trash"] = trash.path
        } else if action == "restore" {
            let original = request["original"] as! String
            let trash = request["trash"] as! String
            if exists(original) {
                throw NSError(domain: NSCocoaErrorDomain, code: NSFileWriteFileExistsError,
                              userInfo: [NSFilePathErrorKey: original])
            }
            try fm.moveItem(atPath: trash, toPath: original)
        } else {
            throw NSError(domain: "GifilesTrash", code: 2)
        }
        response["ok"] = true
    } catch {
        response["ok"] = false
        response["error"] = (error as NSError).localizedDescription
    }
    let data = try! JSONSerialization.data(withJSONObject: response, options: [.sortedKeys])
    print("GIFILES_TRASH " + String(data: data, encoding: .utf8)!)
    fflush(stdout)
}
)SWIFT");
}

bool isLocation(const QString &text) { return text.startsWith(prefix); }

struct Session::Impl {
    struct Connection { QProcess process; bool failed = false; };
    std::optional<QList<Mount>> mounts;
    Launcher launcher;
    bool verifyMounts = false;
    std::map<QString, std::unique_ptr<Connection>> connections;

    bool call(const Mount &mount, QJsonObject request, QJsonObject *reply, QString *error)
    {
        const QString id = QUuid::createUuid().toString(QUuid::Id128);
        request.insert(QStringLiteral("id"), id);
        request.insert(QStringLiteral("v"), 1);
        auto &connection = connections[endpoint(mount)];
        if (!connection) {
            connection = std::make_unique<Connection>();
            auto &process = connection->process;
            if (launcher)
                launcher(process, mount, helperSource());
            else {
                QStringList args{QStringLiteral("-T"), QStringLiteral("-oBatchMode=yes"), QStringLiteral("-oConnectTimeout=5"),
                    QStringLiteral("-oServerAliveInterval=15"), QStringLiteral("-oServerAliveCountMax=2")};
                args << mount.options << QStringLiteral("--") << mount.host
                     << (QStringLiteral("/usr/bin/swift -e ") + shellQuote(helperSource()));
                process.start(QStringLiteral("/usr/bin/ssh"), args);
            }
            if (!process.waitForStarted(10000)) {
                connection->failed = true;
                *error = process.errorString();
                return false;
            }
        }
        auto &process = connection->process;
        if (!connection->failed && process.state() != QProcess::NotRunning) {
            const QByteArray input = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
            if (process.write(input) == input.size()) {
                // No automatic retry after a lost response: the server may already have moved it.
                while (process.state() != QProcess::NotRunning || process.bytesAvailable()) {
                    while (process.canReadLine()) {
                        const QByteArray line = process.readLine();
                        if (!line.startsWith(marker))
                            continue; // e.g. a login banner
                        const auto object = QJsonDocument::fromJson(line.mid(marker.size())).object();
                        if (object.value(QStringLiteral("v")).toInt() != 1 || object.value(QStringLiteral("id")).toString() != id)
                            continue;
                        *reply = object;
                        if (object.value(QStringLiteral("ok")).toBool())
                            return true;
                        *error = object.value(QStringLiteral("error")).toString();
                        return false;
                    }
                    if (!process.waitForReadyRead(60000) && !process.canReadLine())
                        break;
                }
            }
        }
        connection->failed = true;
        *error = Gifiles::tr("서버 휴지통 응답을 확인하지 못했습니다. 자동으로 다시 시도하지 않습니다.");
        const QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
        if (!detail.isEmpty())
            *error += QLatin1Char('\n') + detail;
        return false;
    }
};

Session::Session(std::optional<QList<Mount>> mounts, Launcher launcher) : d(std::make_unique<Impl>())
{
    d->verifyMounts = !mounts.has_value();
    d->mounts = std::move(mounts);
    d->launcher = std::move(launcher);
}

Session::~Session()
{
    for (auto &[key, connection] : d->connections) {
        auto &process = connection->process;
        process.closeWriteChannel();
        if (!process.waitForFinished(1000)) {
            process.kill();
            process.waitForFinished(1000);
        }
    }
}

Result Session::trash(const QString &path, QString *location, QString *error)
{
    if (d->verifyMounts || !d->mounts)
        d->mounts = mountedSshfs();
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const Mount *chosen = nullptr;
    for (const auto &mount : *d->mounts)
        if (util::isInside(absolute, mount.localRoot) && (!chosen || mount.localRoot.size() > chosen->localRoot.size()))
            chosen = &mount;
    if (!chosen || (d->verifyMounts && util::volumeRoot(absolute) != chosen->localRoot))
        return Result::NotApplicable;
    int matches = 0;
    for (const auto &mount : *d->mounts)
        matches += mount.localRoot == chosen->localRoot;
    if (matches != 1 || !chosen->error.isEmpty()) {
        *error = Gifiles::tr("이 SSHFS 연결의 휴지통을 사용할 수 없습니다.");
        return Result::Failed;
    }
    const QString relative = QDir(chosen->localRoot).relativeFilePath(absolute);
    if (relative == QLatin1String(".") || relative == QLatin1String("..") || relative.startsWith(QLatin1String("../"))) {
        *error = Gifiles::tr("마운트 위치 자체는 휴지통으로 옮길 수 없습니다.");
        return Result::Failed;
    }
    const QString remote = chosen->remoteRoot.isEmpty() ? relative : QDir(chosen->remoteRoot).filePath(relative);
    QJsonObject reply;
    if (!d->call(*chosen, {{QStringLiteral("action"), QStringLiteral("trash")}, {QStringLiteral("path"), remote}}, &reply, error))
        return Result::Failed;
    const QString original = reply.value(QStringLiteral("original")).toString();
    const QString trash = reply.value(QStringLiteral("trash")).toString();
    if (!QDir::isAbsolutePath(original) || !QDir::isAbsolutePath(trash)) {
        *error = Gifiles::tr("서버 휴지통 기록이 올바르지 않습니다.");
        return Result::Failed;
    }
    QJsonObject receipt{{QStringLiteral("v"), 1}, {QStringLiteral("host"), chosen->host},
        {QStringLiteral("options"), QJsonArray::fromStringList(chosen->options)},
        {QStringLiteral("request"), remote}, {QStringLiteral("local"), absolute}, {QStringLiteral("original"), original}, {QStringLiteral("trash"), trash}};
    *location = prefix + QString::fromLatin1(QJsonDocument(receipt).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding));
    return Result::Success;
}

bool Session::restore(const QString &location, const QString &destination, QString *error)
{
    const auto receipt = QJsonDocument::fromJson(QByteArray::fromBase64(location.mid(prefix.size()).toLatin1(), QByteArray::Base64UrlEncoding)).object();
    const QString local = receipt.value(QStringLiteral("local")).toString();
    const QString original = receipt.value(QStringLiteral("original")).toString();
    const QString trash = receipt.value(QStringLiteral("trash")).toString();
    const QString host = receipt.value(QStringLiteral("host")).toString();
    if (!isLocation(location) || receipt.value(QStringLiteral("v")).toInt() != 1 || local.isEmpty() ||
        host.isEmpty() || host.startsWith(QLatin1Char('-')) || host.contains(QLatin1Char('\n')) ||
        !QDir::isAbsolutePath(original) || !QDir::isAbsolutePath(trash) || QDir::cleanPath(destination) != local) {
        *error = Gifiles::tr("서버 휴지통 기록이 올바르지 않습니다.");
        return false;
    }
    Mount mount;
    mount.host = host;
    for (const auto &option : receipt.value(QStringLiteral("options")).toArray())
        mount.options << option.toString();
    if (d->verifyMounts || !d->mounts)
        d->mounts = mountedSshfs();
    const Mount *current = nullptr;
    for (const auto &candidate : *d->mounts)
        if (util::isInside(destination, candidate.localRoot) && (!current || candidate.localRoot.size() > current->localRoot.size()))
            current = &candidate;
    int matches = 0;
    if (current)
        for (const auto &candidate : *d->mounts)
            matches += candidate.localRoot == current->localRoot;
    const QString relative = current ? QDir(current->localRoot).relativeFilePath(destination) : QString();
    const QString remote = current && !current->remoteRoot.isEmpty() ? QDir(current->remoteRoot).filePath(relative) : relative;
    if (!current || matches != 1 || !current->error.isEmpty() || (d->verifyMounts && util::volumeRoot(destination) != current->localRoot) ||
        endpoint(*current) != endpoint(mount) ||
        remote != receipt.value(QStringLiteral("request")).toString()) {
        *error = Gifiles::tr("서버 휴지통 기록이 올바르지 않습니다.");
        return false;
    }
    QJsonObject reply;
    return d->call(*current, {{QStringLiteral("action"), QStringLiteral("restore")},
        {QStringLiteral("original"), original}, {QStringLiteral("trash"), trash}}, &reply, error);
}
} // namespace SshTrash
