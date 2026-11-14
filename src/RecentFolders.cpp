#include "RecentFolders.h"

#include "FileOps.h"
#include "Settings.h"
#include "Util.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

namespace {
const QString kKey = QStringLiteral("recent/folders");

QString folderIdentity(const QString &path)
{
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    // Never resolve/stat an SSH mount just to update the sidebar. Work already happened there.
    QString identity = absolute;
    if (!util::usesGenericFileIcon(absolute)) {
        const QString canonical = QFileInfo(absolute).canonicalFilePath();
        if (!canonical.isEmpty())
            identity = canonical;
    }
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    return identity.normalized(QString::NormalizationForm_C);
}

QString parentOf(const QString &path)
{
    return QFileInfo(QDir::cleanPath(path)).absolutePath();
}
} // namespace

RecentFolders *RecentFolders::instance()
{
    static RecentFolders *r = new RecentFolders;
    return r;
}

RecentFolders::RecentFolders()
{
    connect(Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key.isEmpty() || key == QLatin1String(Settings::RecentFoldersCount))
            emit changed();
    });
}

QStringList RecentFolders::stored() const
{
    const QStringList saved = QSettings().value(kKey).toStringList();
    QStringList unique;
    QSet<QString> seen;
    for (const QString &path : saved) {
        if (path.isEmpty())
            continue;
        const QString id = folderIdentity(path);
        if (!seen.contains(id)) {
            seen.insert(id);
            unique << QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        }
    }
    // Migrate old alias duplicates while preserving the most recent display path/order.
    unique = unique.mid(0, kKept);
    if (unique != saved)
        QSettings().setValue(kKey, unique);
    return unique;
}

void RecentFolders::store(const QStringList &list)
{
    QSettings().setValue(kKey, list.mid(0, kKept));
    emit changed();
}

void RecentFolders::noteAll(const QStringList &folders)
{
    // The last one noted is the newest (a move: the source, then the destination on top).
    QStringList list = stored();
    const QStringList before = list;
    for (const QString &raw : folders) {
        if (raw.isEmpty())
            continue;
        const QString folder = QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
        if (!util::usesGenericFileIcon(folder) && !QFileInfo(folder).isDir())
            continue;
        const QString id = folderIdentity(folder);
        list.removeIf([&](const QString &other) { return folderIdentity(other) == id; });
        list.prepend(folder);
    }
    if (list != before)
        store(list);
}

void RecentFolders::note(const QString &folder)
{
    noteAll({folder});
}

void RecentFolders::noteRecord(const UndoRecord &rec)
{
    QStringList folders;
    for (const Step &s : rec.steps) {
        switch (s.kind) {
        case Step::Move: // moved out of one folder into another (a rename: the same folder)
            folders << parentOf(s.from) << parentOf(s.to);
            break;
        case Step::Trash: // deleted from where it was
            folders << parentOf(s.from);
            break;
        case Step::Copy: // the copy is the work; the source was only read
        case Step::Create:
        case Step::Mkdir:
        case Step::Extract:
            folders << parentOf(s.to);
            break;
        }
    }
    noteAll(folders);
}

bool RecentFolders::isNavigationCommand(const QString &line)
{
    const QString first = line.trimmed().section(QRegularExpression(QStringLiteral("[\\s;|&]")), 0, 0).toLower();
    static const QSet<QString> looking = {
        QStringLiteral("cd"),  QStringLiteral("pushd"), QStringLiteral("popd"),         QStringLiteral("ls"),
        QStringLiteral("ll"),  QStringLiteral("la"),    QStringLiteral("l"),            QStringLiteral("dir"),
        QStringLiteral("pwd"), QStringLiteral("clear"), QStringLiteral("cls"),          QStringLiteral("exit"),
        QStringLiteral("z"),   QStringLiteral("set-location"), QStringLiteral("sl"),    QStringLiteral("get-childitem"),
        QStringLiteral("gci"), QStringLiteral("get-location"), QStringLiteral("gl"),    QStringLiteral("clear-host"),
    };
    return first.isEmpty() || looking.contains(first);
}

void RecentFolders::noteCommand(const QString &folder, const QString &line)
{
    if (!line.isEmpty() && isNavigationCommand(line))
        return;
    note(folder);
}

void RecentFolders::remove(const QString &folder)
{
    QStringList list = stored();
    const QString id = folderIdentity(folder);
    if (list.removeIf([&](const QString &other) { return folderIdentity(other) == id; }))
        store(list);
}

void RecentFolders::clear()
{
    if (!stored().isEmpty())
        store({});
}

QStringList RecentFolders::folders() const
{
    const int count = qBound(0, Settings::instance()->value(Settings::RecentFoldersCount).toInt(), kKept);
    QStringList out;
    for (const QString &f : stored()) {
        if (out.size() >= count)
            break;
        if (util::usesGenericFileIcon(f) || QFileInfo(f).isDir())
            out << f;
    }
    return out;
}
