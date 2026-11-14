#pragma once

#include <QList>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

class QProcess;

// SSHFS exposes file operations, but cannot invoke the server's native Trash API. A job owns
// one session per SSH endpoint; paths travel as JSON on stdin, never as shell commands.
namespace SshTrash {
struct Mount {
    QString localRoot;
    QString host;
    QString remoteRoot; // empty/relative means relative to the server user's home
    QStringList options;
    QString error;
};
std::optional<Mount> parseMount(const QStringList &argv);
QList<Mount> mountedSshfs();
QString helperSource();
bool isLocation(const QString &text);

enum class Result { NotApplicable, Success, Failed };
class Session {
public:
    // The launcher seam lets tests exercise the exact native helper locally without SSH.
    using Launcher = std::function<void(QProcess &, const Mount &, const QString &)>;
    explicit Session(std::optional<QList<Mount>> mounts = std::nullopt, Launcher launcher = {});
    ~Session();
    Result trash(const QString &path, QString *location, QString *error);
    bool restore(const QString &location, const QString &destination, QString *error);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace SshTrash
