#pragma once

#include <QByteArray>
#include <QDir>
#include <QList>
#include <QString>

// mountinfo is kernel metadata: parsing it must never stat/open the mounted filesystem.
namespace LinuxMounts {
struct Mount {
    QString root;
    bool remote;
    QByteArray type;
};

inline QByteArray unescape(const QByteArray &text)
{
    QByteArray out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size()
            && text[i + 1] >= '0' && text[i + 1] <= '7'
            && text[i + 2] >= '0' && text[i + 2] <= '7'
            && text[i + 3] >= '0' && text[i + 3] <= '7') {
            out += char((text[i + 1] - '0') * 64 + (text[i + 2] - '0') * 8 + text[i + 3] - '0');
            i += 3;
        } else {
            out += text[i];
        }
    }
    return out;
}

inline QList<Mount> parse(const QByteArray &data)
{
    QList<Mount> out;
    for (const auto &line : data.split('\n')) {
        const auto fields = line.split(' ');
        const int separator = fields.indexOf(QByteArray("-"));
        if (separator < 6 || separator + 3 >= fields.size())
            continue;
        const auto &type = fields[separator + 1];
        // FUSE includes rclone, SSHFS and GVfs. Even a local FUSE daemon may block;
        // generic icons are preferable to synchronous content sniffing on the GUI thread.
        const bool remote = type == "fuse" || type.startsWith("fuse.") || type == "autofs"
            || type == "nfs" || type == "nfs4" || type == "cifs" || type == "smb3"
            || type == "9p" || type == "ceph" || type == "afs" || type == "davfs";
        out.append({QString::fromUtf8(unescape(fields[4])), remote, type});
    }
    return out;
}

inline Mount containing(const QList<Mount> &mounts, const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    Mount best{QStringLiteral("/"), false, {}};
    for (const auto &mount : mounts)
        if (mount.root.size() >= best.root.size()
            && (clean == mount.root || clean.startsWith(mount.root == QLatin1String("/")
                ? mount.root : mount.root + QLatin1Char('/'))))
            best = mount;
    return best;
}
} // namespace LinuxMounts
