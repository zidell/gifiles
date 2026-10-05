// Unit tests for the file-operation engine (src/FileOps): copy, move, duplicate, trash, extract,
// undo/redo replay, rename and new-folder helpers. Everything happens inside a QTemporaryDir;
// items that go to the real trash carry a per-run token in their names and are restored by undo
// where the test allows, the rest is purged from the trash at the end (best effort, macOS/Linux).

#include "Headless.h"

#include "FileOps.h"
#include "Util.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>

#include <functional>
#include <memory>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

const QDir::Filters kAll = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;

OpResult run(const Job &j)
{
    QFuture<OpResult> f = FileOps::start(j);
    f.waitForFinished();
    return f.result();
}

Job makeJob(Job::Type t, const QStringList &sources, const QString &dest = {})
{
    Job j;
    j.type = t;
    j.sources = sources;
    j.destDir = dest;
    return j;
}

Job replayJob(Job::Type t, const UndoRecord &rec)
{
    Job j;
    j.type = t;
    j.record = rec;
    return j;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
}

// Relative path → content ("<dir>" for folders, "link:<target>" for links); links are not followed.
QMap<QString, QByteArray> snapshot(const QString &root)
{
    QMap<QString, QByteArray> m;
    const QDir base(root);
    QDirIterator it(root, kAll, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi = it.nextFileInfo();
        const QString rel = base.relativeFilePath(fi.filePath());
        if (fi.isSymLink())
            m[rel] = "link:" + fi.readSymLink().toUtf8();
        else if (fi.isDir())
            m[rel] = "<dir>";
        else
            m[rel] = readFile(fi.filePath());
    }
    return m;
}

QStringList names(const QString &dir) // sorted by code point, independent of the locale
{
    QStringList l = QDir(dir).entryList(kAll, QDir::NoSort);
    l.sort();
    return l;
}

QStringList leftovers(const QString &dir)
{
    return QDir(dir).entryList({QStringLiteral(".gifiles-extract-*")}, kAll);
}

bool posixPermsEnforced()
{
#ifdef Q_OS_UNIX
    return geteuid() != 0;
#else
    return false;
#endif
}

// Changes permissions for the scope of a test and puts them back, so QTemporaryDir can clean up.
struct PermGuard {
    QString path;
    QFile::Permissions old;
    PermGuard(const QString &p, QFile::Permissions perms)
        : path(p), old(QFile::permissions(p))
    {
        QFile::setPermissions(path, perms);
    }
    ~PermGuard() { QFile::setPermissions(path, old); }
};
const QFile::Permissions kReadOnlyDir = QFile::ReadOwner | QFile::ExeOwner | QFile::ReadUser | QFile::ExeUser;

struct ScopeExit {
    std::function<void()> fn;
    ~ScopeExit() { fn(); }
};

// ---- hand-made archives (no tool needed to create them) ----

struct Entry {
    QString name;
    QByteArray data;
    bool dir = false;
};

QByteArray makeTar(const QList<Entry> &entries)
{
    QByteArray out;
    for (const Entry &e : entries) {
        QByteArray h(512, '\0');
        QByteArray name = e.name.toUtf8();
        if (e.dir && !name.endsWith('/'))
            name += '/';
        memcpy(h.data(), name.constData(), qMin<qsizetype>(name.size(), 100));
        auto octal = [&h](int off, int len, qint64 v) {
            const QByteArray s = QByteArray::number(v, 8).rightJustified(len - 1, '0');
            memcpy(h.data() + off, s.constData(), len - 1);
        };
        octal(100, 8, e.dir ? 0755 : 0644);
        octal(108, 8, 0);
        octal(116, 8, 0);
        octal(124, 12, e.dir ? 0 : e.data.size());
        octal(136, 12, 1700000000);
        h[156] = e.dir ? '5' : '0';
        memcpy(h.data() + 257, "ustar\0" "00", 8);
        memset(h.data() + 148, ' ', 8);
        unsigned sum = 0;
        for (char c : h)
            sum += uchar(c);
        const QByteArray cs = QByteArray::number(sum, 8).rightJustified(6, '0');
        memcpy(h.data() + 148, cs.constData(), 6);
        h[154] = '\0';
        h[155] = ' ';
        out += h;
        if (!e.dir) {
            out += e.data;
            out += QByteArray((512 - e.data.size() % 512) % 512, '\0');
        }
    }
    out += QByteArray(1024, '\0');
    return out;
}

quint32 crc32(const QByteArray &d)
{
    quint32 c = 0xFFFFFFFFu;
    for (char ch : d) {
        c ^= uchar(ch);
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

QByteArray makeZip(const QList<Entry> &entries) // "stored" entries, no compression
{
    QByteArray out, central;
    auto le16 = [](QByteArray &b, quint16 v) {
        b.append(char(v & 0xff));
        b.append(char(v >> 8));
    };
    auto le32 = [](QByteArray &b, quint32 v) {
        for (int i = 0; i < 4; ++i)
            b.append(char((v >> (8 * i)) & 0xff));
    };
    for (const Entry &e : entries) {
        QByteArray name = e.name.toUtf8();
        if (e.dir && !name.endsWith('/'))
            name += '/';
        const QByteArray data = e.dir ? QByteArray() : e.data;
        const quint32 crc = crc32(data), size = quint32(data.size()), offset = quint32(out.size());
        le32(out, 0x04034b50);
        le16(out, 20);
        le16(out, 0);
        le16(out, 0);
        le16(out, 0);
        le16(out, 0x21);
        le32(out, crc);
        le32(out, size);
        le32(out, size);
        le16(out, quint16(name.size()));
        le16(out, 0);
        out += name;
        out += data;
        le32(central, 0x02014b50);
        le16(central, 20);
        le16(central, 20);
        le16(central, 0);
        le16(central, 0);
        le16(central, 0);
        le16(central, 0x21);
        le32(central, crc);
        le32(central, size);
        le32(central, size);
        le16(central, quint16(name.size()));
        le16(central, 0);
        le16(central, 0);
        le16(central, 0);
        le16(central, 0);
        le32(central, e.dir ? 0x10 : 0);
        le32(central, offset);
        central += name;
    }
    const quint32 cdOffset = quint32(out.size());
    out += central;
    le32(out, 0x06054b50);
    le16(out, 0);
    le16(out, 0);
    le16(out, quint16(entries.size()));
    le16(out, quint16(entries.size()));
    le32(out, quint32(central.size()));
    le32(out, cdOffset);
    le16(out, 0);
    return out;
}

// The tool src/FileOps.cpp (extractArchive) runs for this kind of archive.
bool haveExtractor(bool zip)
{
#if defined(Q_OS_MACOS)
    return QFileInfo::exists(zip ? QStringLiteral("/usr/bin/ditto") : QStringLiteral("/usr/bin/tar"));
#elif defined(Q_OS_WIN)
    Q_UNUSED(zip);
    return !QStandardPaths::findExecutable(QStringLiteral("tar")).isEmpty();
#else
    return !QStandardPaths::findExecutable(zip ? QStringLiteral("unzip") : QStringLiteral("tar")).isEmpty();
#endif
}

// A writable folder on another volume than `here`, for the cross-volume move fallback.
std::unique_ptr<QTemporaryDir> otherVolumeDir(const QString &here)
{
    QStringList candidates;
    if (qEnvironmentVariableIsSet("GIFILES_TEST_OTHER_VOLUME"))
        candidates << qEnvironmentVariable("GIFILES_TEST_OTHER_VOLUME");
#if defined(Q_OS_LINUX)
    candidates << QStringLiteral("/dev/shm") << QStringLiteral("/run/user/%1").arg(getuid());
#elif defined(Q_OS_WIN)
    for (const QStorageInfo &v : QStorageInfo::mountedVolumes())
        if (v.isValid() && v.isReady() && !v.isReadOnly())
            candidates << v.rootPath();
#endif
    const QByteArray dev = QStorageInfo(here).device();
    for (const QString &c : std::as_const(candidates)) {
        if (!QFileInfo(c).isDir() || QStorageInfo(c).device() == dev)
            continue;
        auto t = std::make_unique<QTemporaryDir>(QDir(c).filePath(QStringLiteral("gifiles-xvol-XXXXXX")));
        if (t->isValid())
            return t;
    }
    return nullptr;
}

} // namespace

class Unit : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_dir;   // this test's own folder
    QString m_token; // marks items that may end up in the real trash
    bool m_canTrash = false;
    QStringList m_trashed;    // known locations inside the trash, removed at the end
    QStringList m_trashNames; // names trashed without a reported location (undo of a copy)

    QString d(const QString &rel) const { return QDir(m_dir).filePath(rel); }
    QString mk(const QString &rel)
    {
        QDir().mkpath(d(rel));
        return d(rel);
    }
    QString file(const QString &rel, const QByteArray &data = "x")
    {
        QDir().mkpath(QFileInfo(d(rel)).absolutePath());
        writeFile(d(rel), data);
        return d(rel);
    }
    // "old.txt" → "old-<token>.txt": a name that can be found again in the trash.
    QString T(const QString &name) const
    {
        QString base, ext;
        util::splitExt(name, &base, &ext);
        return base + QLatin1Char('-') + m_token + ext;
    }
    void track(const UndoRecord &rec)
    {
        for (const Step &s : rec.steps)
            if (s.kind == Step::Trash)
                m_trashed << s.to;
    }
    static void removeAny(const QString &p)
    {
        const QFileInfo fi(p);
        if (fi.isDir() && !fi.isSymLink())
            QDir(p).removeRecursively();
        else if (util::exists(p))
            QFile::remove(p);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_token = QUuid::createUuid().toString(QUuid::Id128).left(10);
        const QString probe = m_tmp.filePath(T(QStringLiteral("probe.txt")));
        QVERIFY(writeFile(probe, "p"));
        QFile f(probe);
        m_canTrash = f.moveToTrash();
        if (m_canTrash)
            m_trashed << f.fileName();
        else
            qWarning("moving to the trash does not work here; trash tests are skipped");
    }

    void cleanupTestCase()
    {
        for (const QString &p : std::as_const(m_trashed))
            removeAny(p);
        // Items whose trash location was not reported (undo of a copy trashes it): find by token.
        QStringList trashDirs;
#if defined(Q_OS_MACOS)
        trashDirs << QDir::home().filePath(QStringLiteral(".Trash"));
#elif defined(Q_OS_UNIX)
        trashDirs << QDir::home().filePath(QStringLiteral(".local/share/Trash/files"))
                  << QDir::home().filePath(QStringLiteral(".local/share/Trash/info"))
                  << QDir(QStorageInfo(m_tmp.path()).rootPath()).filePath(QStringLiteral(".Trash-%1/files").arg(getuid()))
                  << QDir(QStorageInfo(m_tmp.path()).rootPath()).filePath(QStringLiteral(".Trash-%1/info").arg(getuid()));
#endif
        for (const QString &t : std::as_const(trashDirs)) {
            for (const QString &n : QDir(t).entryList(kAll)) // may be unreadable (macOS privacy)
                if (n.contains(m_token))
                    removeAny(QDir(t).filePath(n));
            for (const QString &n : std::as_const(m_trashNames))
                removeAny(QDir(t).filePath(n));
        }
    }

    void init()
    {
        m_dir = m_tmp.filePath(QString::fromLatin1(QTest::currentTestFunction()));
        QVERIFY(QDir().mkpath(m_dir));
    }

    void cleanup() { QDir(m_dir).removeRecursively(); }

    // ---------------------------------------------------------------- helpers in the API

    void verbNamesEveryJobType()
    {
        QSet<QString> seen;
        for (Job::Type t : {Job::Copy, Job::Move, Job::Trash, Job::Duplicate, Job::Extract, Job::Undo, Job::Redo}) {
            const QString v = FileOps::verb(t);
            QVERIFY(!v.isEmpty());
            seen.insert(v);
        }
        QCOMPARE(seen.size(), 7);
    }

    void isArchiveRecognizesSuffixes()
    {
        for (const char *n : {"a.zip", "b.TAR.GZ", "c.tgz", "d.7z", "e.rar", "f.tar.zst", "g.tbz2", "h.cpio", "i.xar"})
            QVERIFY2(FileOps::isArchive(file(QString::fromLatin1(n))), n);
        QVERIFY(!FileOps::isArchive(file(QStringLiteral("notes.txt"))));
        QVERIFY(!FileOps::isArchive(file(QStringLiteral("zip"))));
        QVERIFY(!FileOps::isArchive(mk(QStringLiteral("folder.zip"))));   // folders are never archives
        QVERIFY(!FileOps::isArchive(d(QStringLiteral("missing.zip")))); // nor missing files
    }

    // ---------------------------------------------------------------- copy

    void copyFileIntoOtherFolder()
    {
        const QString src = file(QStringLiteral("src/a.txt"), "hello");
        const QString dst = mk(QStringLiteral("dst"));
        QFuture<OpResult> f = FileOps::start(makeJob(Job::Copy, {src}, dst));
        f.waitForFinished();
        const OpResult r = f.result();
        const QString target = QDir(dst).filePath(QStringLiteral("a.txt"));
        QVERIFY(r.errors.isEmpty());
        QVERIFY(!r.canceled);
        QCOMPARE(r.type, Job::Copy);
        QCOMPARE(r.destDir, dst);
        QCOMPARE(r.created, QStringList{target});
        QCOMPARE(readFile(target), QByteArray("hello"));
        QCOMPARE(readFile(src), QByteArray("hello"));
        QCOMPARE(r.record.label, FileOps::verb(Job::Copy));
        QCOMPARE(r.record.steps.size(), 1);
        QCOMPARE(r.record.steps[0].kind, Step::Copy);
        QCOMPARE(r.record.steps[0].from, src);
        QCOMPARE(r.record.steps[0].to, target);
        QCOMPARE(f.progressMaximum(), 1);
        QCOMPARE(f.progressValue(), 1);
    }

    void copyFolderRecursively()
    {
        QByteArray binary;
        for (int i = 0; i < 256; ++i)
            binary.append(char(i));
        const QString src = mk(QStringLiteral("src/tree"));
        file(QStringLiteral("src/tree/a.txt"), "A");
        file(QStringLiteral("src/tree/.hidden"), "H");
        file(QStringLiteral("src/tree/sub/deep/b.bin"), binary);
        mk(QStringLiteral("src/tree/empty"));
        const QString dst = mk(QStringLiteral("dst"));

        QFuture<OpResult> f = FileOps::start(makeJob(Job::Copy, {src}, dst));
        f.waitForFinished();
        const OpResult r = f.result();
        const QString target = QDir(dst).filePath(QStringLiteral("tree"));
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.created, QStringList{target});
        QCOMPARE(snapshot(target), snapshot(src));
        QCOMPARE(snapshot(target).size(), 6);
        // Progress counts the folder itself plus every entry inside, one tick per copied item.
        QCOMPARE(f.progressMaximum(), 7);
        QCOMPARE(f.progressValue(), 7);
    }

    void copyIntoSameFolderKeepsBoth()
    {
        const QString src = file(QStringLiteral("a.txt"), "orig");
        OpResult r = run(makeJob(Job::Copy, {src}, m_dir));
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.created, QStringList{d(QStringLiteral("a 복사본.txt"))});

        // A copy into its own folder never replaces the original, whatever the decision says.
        Job j = makeJob(Job::Copy, {src}, m_dir);
        j.conflicts.insert(src, Conflict::Replace);
        r = run(j);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.created, QStringList{d(QStringLiteral("a 복사본 2.txt"))});
        QCOMPARE(names(m_dir), (QStringList{QStringLiteral("a 복사본 2.txt"), QStringLiteral("a 복사본.txt"), QStringLiteral("a.txt")}));
        QCOMPARE(readFile(src), QByteArray("orig"));
        for (const Step &s : r.record.steps)
            QVERIFY(s.kind != Step::Trash);
    }

    void copyConflictKeepsBothByDefault()
    {
        const QString src = file(QStringLiteral("src/a.txt"), "new");
        const QString old = file(QStringLiteral("dst/a.txt"), "old");
        const OpResult r = run(makeJob(Job::Copy, {src}, d(QStringLiteral("dst"))));
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.created, QStringList{d(QStringLiteral("dst/a 복사본.txt"))});
        QCOMPARE(readFile(old), QByteArray("old"));
        QCOMPARE(readFile(d(QStringLiteral("dst/a 복사본.txt"))), QByteArray("new"));
    }

    void copyConflictSkipLeavesTarget()
    {
        const QString src = file(QStringLiteral("src/a.txt"), "new");
        const QString old = file(QStringLiteral("dst/a.txt"), "old");
        const QString other = file(QStringLiteral("src/b.txt"), "b");
        Job j = makeJob(Job::Copy, {src, other}, d(QStringLiteral("dst")));
        j.conflicts.insert(src, Conflict::Skip);
        const OpResult r = run(j);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.created, QStringList{d(QStringLiteral("dst/b.txt"))});
        QCOMPARE(r.record.steps.size(), 1);
        QCOMPARE(readFile(old), QByteArray("old"));
        QCOMPARE(names(d(QStringLiteral("dst"))), (QStringList{QStringLiteral("a.txt"), QStringLiteral("b.txt")}));
    }

    void copyConflictReplaceTrashesOldAndUndoRedo()
    {
        if (!m_canTrash)
            QSKIP("no trash on this system");
        const QString name = T(QStringLiteral("a.txt"));
        const QString src = file(QStringLiteral("src/") + name, "new");
        const QString target = file(QStringLiteral("dst/") + name, "old");
        Job j = makeJob(Job::Copy, {src}, d(QStringLiteral("dst")));
        j.conflicts.insert(src, Conflict::Replace);
        const OpResult r = run(j);
        track(r.record);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.created, QStringList{target});
        QCOMPARE(readFile(target), QByteArray("new"));
        QCOMPARE(r.record.steps.size(), 2);
        QCOMPARE(r.record.steps[0].kind, Step::Trash);
        QCOMPARE(r.record.steps[0].from, target);
        QCOMPARE(readFile(r.record.steps[0].to), QByteArray("old")); // in the trash, not deleted
        QCOMPARE(r.record.steps[1].kind, Step::Copy);

        // Undo: the copy goes to the trash and the replaced item comes back.
        m_trashNames << name;
        const OpResult u = run(replayJob(Job::Undo, r.record));
        QVERIFY2(u.errors.isEmpty(), qPrintable(u.errors.join(QLatin1Char('\n'))));
        QCOMPARE(readFile(target), QByteArray("old"));
        QVERIFY(!util::exists(r.record.steps[0].to));
        QVERIFY(u.created.contains(target));
        QCOMPARE(readFile(src), QByteArray("new"));

        // Redo: the old item goes to the trash again (new location recorded), the copy is made again.
        const OpResult re = run(replayJob(Job::Redo, u.record));
        track(re.record);
        QVERIFY2(re.errors.isEmpty(), qPrintable(re.errors.join(QLatin1Char('\n'))));
        QCOMPARE(readFile(target), QByteArray("new"));
        QCOMPARE(readFile(re.record.steps[0].to), QByteArray("old"));

        // And undo once more to leave nothing of value in the trash.
        const OpResult u2 = run(replayJob(Job::Undo, re.record));
        QVERIFY(u2.errors.isEmpty());
        QCOMPARE(readFile(target), QByteArray("old"));
    }

    void copyFolderIntoItselfIsRefused()
    {
        const QString src = mk(QStringLiteral("box"));
        file(QStringLiteral("box/sub/a.txt"), "a");
        const auto before = snapshot(m_dir);
        for (const QString &dest : {src, d(QStringLiteral("box/sub"))}) {
            for (Job::Type t : {Job::Copy, Job::Move}) {
                const OpResult r = run(makeJob(t, {src}, dest));
                QCOMPARE(r.errors.size(), 1);
                QVERIFY(r.record.isEmpty());
                QVERIFY(r.created.isEmpty());
                QCOMPARE(snapshot(m_dir), before);
            }
        }
    }

    void copyMissingSourceReportsError()
    {
        const QString dst = mk(QStringLiteral("dst"));
        const OpResult r = run(makeJob(Job::Copy, {d(QStringLiteral("missing.txt"))}, dst));
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.record.isEmpty());
        QVERIFY(names(dst).isEmpty());
    }

    void copyUnreadableFileDropsPartialCopy()
    {
        if (!posixPermsEnforced())
            QSKIP("needs enforced POSIX permissions (not Windows, not root)");
        const QString src = mk(QStringLiteral("src/folder"));
        file(QStringLiteral("src/folder/a.txt"), "a");
        const QString locked = file(QStringLiteral("src/folder/b.txt"), "b");
        file(QStringLiteral("src/folder/c.txt"), "c");
        const QString dst = mk(QStringLiteral("dst"));
        PermGuard g(locked, {});
        const OpResult r = run(makeJob(Job::Copy, {src}, dst));
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.errors[0].contains(QStringLiteral("b.txt")));
        QVERIFY(r.record.isEmpty());
        QVERIFY(r.created.isEmpty());
        QVERIFY2(names(dst).isEmpty(), "the partial copy must be removed");
        QCOMPARE(names(src).size(), 3);

        // The same for a single file.
        const OpResult r2 = run(makeJob(Job::Copy, {locked}, dst));
        QCOMPARE(r2.errors.size(), 1);
        QVERIFY(names(dst).isEmpty());
    }

    void copyIntoReadOnlyFolderFails()
    {
        if (!posixPermsEnforced())
            QSKIP("needs enforced POSIX permissions (not Windows, not root)");
        const QString folder = mk(QStringLiteral("src/folder"));
        file(QStringLiteral("src/folder/a.txt"), "a");
        const QString single = file(QStringLiteral("src/b.txt"), "b");
        const QString dst = mk(QStringLiteral("dst"));
        PermGuard g(dst, kReadOnlyDir);
        const OpResult r = run(makeJob(Job::Copy, {folder, single}, dst));
        QCOMPARE(r.errors.size(), 2);
        QVERIFY(r.record.isEmpty());
        QVERIFY(names(dst).isEmpty());
        QCOMPARE(readFile(single), QByteArray("b"));
    }

    void copyReplaceThenFailureKeepsOldRecoverable()
    {
        if (!m_canTrash)
            QSKIP("no trash on this system");
        if (!posixPermsEnforced())
            QSKIP("needs enforced POSIX permissions (not Windows, not root)");
        const QString name = T(QStringLiteral("a.txt"));
        const QString src = file(QStringLiteral("src/") + name, "new");
        const QString target = file(QStringLiteral("dst/") + name, "old");
        OpResult r;
        {
            PermGuard g(src, {});
            Job j = makeJob(Job::Copy, {src}, d(QStringLiteral("dst")));
            j.conflicts.insert(src, Conflict::Replace);
            r = run(j);
        }
        track(r.record);
        QCOMPARE(r.errors.size(), 1);
        // The replaced item is in the trash and the record says so, so undo brings it back.
        QCOMPARE(r.record.steps.size(), 1);
        QCOMPARE(r.record.steps[0].kind, Step::Trash);
        QVERIFY(!util::exists(target));
        const OpResult u = run(replayJob(Job::Undo, r.record));
        QVERIFY(u.errors.isEmpty());
        QCOMPARE(readFile(target), QByteArray("old"));
    }

    void replaceThatCannotTrashKeepsBoth()
    {
        if (!posixPermsEnforced())
            QSKIP("needs enforced POSIX permissions (not Windows, not root)");
        // The old item can't be moved to the trash (its folder is read-only): nothing is replaced.
        const QString src = file(QStringLiteral("src/a.txt"), "new");
        const QString old = file(QStringLiteral("dst/a.txt"), "old");
        PermGuard g(d(QStringLiteral("dst")), kReadOnlyDir);
        for (Job::Type t : {Job::Copy, Job::Move}) {
            Job j = makeJob(t, {src}, d(QStringLiteral("dst")));
            j.conflicts.insert(src, Conflict::Replace);
            const OpResult r = run(j);
            QCOMPARE(r.errors.size(), 1);
            QVERIFY(r.record.isEmpty());
            QCOMPARE(readFile(old), QByteArray("old"));
            QCOMPARE(readFile(src), QByteArray("new"));
        }
    }

    void copySymlinksAsLinks()
    {
#ifndef Q_OS_UNIX
        QSKIP("symlinks need privileges on Windows");
#else
        const QString outside = mk(QStringLiteral("outside"));
        file(QStringLiteral("outside/big.txt"), "big");
        const QString src = mk(QStringLiteral("src/folder"));
        const QString real = file(QStringLiteral("src/folder/real.txt"), "r");
        QVERIFY(QFile::link(real, d(QStringLiteral("src/folder/to-file"))));
        QVERIFY(QFile::link(outside, d(QStringLiteral("src/folder/to-dir"))));
        QVERIFY(QFile::link(d(QStringLiteral("nowhere")), d(QStringLiteral("src/folder/broken"))));
        const QString dst = mk(QStringLiteral("dst"));

        const OpResult r = run(makeJob(Job::Copy, {src}, dst));
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        const QString copy = QDir(dst).filePath(QStringLiteral("folder"));
        for (const char *n : {"to-file", "to-dir", "broken"})
            QVERIFY2(QFileInfo(QDir(copy).filePath(QString::fromLatin1(n))).isSymLink(), n);
        QCOMPARE(QFileInfo(QDir(copy).filePath(QStringLiteral("to-dir"))).symLinkTarget(), QFileInfo(outside).absoluteFilePath());
        QCOMPARE(QFileInfo(QDir(copy).filePath(QStringLiteral("broken"))).symLinkTarget(), d(QStringLiteral("nowhere")));
        QCOMPARE(names(outside), QStringList{QStringLiteral("big.txt")}); // the linked folder is not copied into

        // A link given as the source is copied as a link, counted as one item.
        QFuture<OpResult> f = FileOps::start(makeJob(Job::Copy, {d(QStringLiteral("src/folder/to-dir"))}, dst));
        f.waitForFinished();
        QVERIFY(f.result().errors.isEmpty());
        QVERIFY(QFileInfo(QDir(dst).filePath(QStringLiteral("to-dir"))).isSymLink());
        QCOMPARE(f.progressMaximum(), 1);
#endif
    }

    void copyKeepsRelativeSymlinksRelative()
    {
#ifndef Q_OS_UNIX
        QSKIP("symlinks need privileges on Windows");
#else
        const QString src = mk(QStringLiteral("src/folder"));
        file(QStringLiteral("src/folder/target.txt"), "original");
        QVERIFY(QFile::link(QStringLiteral("target.txt"), d(QStringLiteral("src/folder/rel"))));
        const QString dst = mk(QStringLiteral("dst"));
        const OpResult r = run(makeJob(Job::Copy, {src}, dst));
        QVERIFY(r.errors.isEmpty());
        const QString copy = QDir(dst).filePath(QStringLiteral("folder"));
        writeFile(QDir(copy).filePath(QStringLiteral("target.txt")), "copy");
        QCOMPARE(QFileInfo(QDir(copy).filePath(QStringLiteral("rel"))).readSymLink(), QStringLiteral("target.txt"));
        QCOMPARE(readFile(QDir(copy).filePath(QStringLiteral("rel"))), QByteArray("copy"));
#endif
    }

    void copyUnreadableSubfolderIsReported()
    {
        if (!posixPermsEnforced())
            QSKIP("needs enforced POSIX permissions (not Windows, not root)");
        const QString src = mk(QStringLiteral("src/folder"));
        file(QStringLiteral("src/folder/a.txt"), "a");
        const QString locked = mk(QStringLiteral("src/folder/locked"));
        file(QStringLiteral("src/folder/locked/secret.txt"), "s");
        const QString dst = mk(QStringLiteral("dst"));
        PermGuard g(locked, {});
        const OpResult r = run(makeJob(Job::Copy, {src}, dst));
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.record.isEmpty());
        QVERIFY(names(dst).isEmpty());
    }

    // ---------------------------------------------------------------- move

    void moveFileAndFolderAndUndoRedo()
    {
        const QString a = file(QStringLiteral("src/a.txt"), "a");
        const QString folder = mk(QStringLiteral("src/folder"));
        file(QStringLiteral("src/folder/inner.txt"), "i");
        const QString dst = mk(QStringLiteral("dst"));
        const auto folderBefore = snapshot(folder);

        const OpResult r = run(makeJob(Job::Move, {a, folder}, dst));
        QVERIFY(r.errors.isEmpty());
        const QString a2 = QDir(dst).filePath(QStringLiteral("a.txt")), folder2 = QDir(dst).filePath(QStringLiteral("folder"));
        QCOMPARE(r.created, (QStringList{a2, folder2}));
        QCOMPARE(r.record.label, FileOps::verb(Job::Move));
        QCOMPARE(r.record.steps.size(), 2);
        QCOMPARE(r.record.steps[0].kind, Step::Move);
        QVERIFY(names(d(QStringLiteral("src"))).isEmpty());
        QCOMPARE(readFile(a2), QByteArray("a"));
        QCOMPARE(snapshot(folder2), folderBefore);

        const OpResult u = run(replayJob(Job::Undo, r.record));
        QVERIFY(u.errors.isEmpty());
        QCOMPARE(u.created, (QStringList{folder, a})); // undone last step first
        QVERIFY(names(dst).isEmpty());
        QCOMPARE(readFile(a), QByteArray("a"));
        QCOMPARE(snapshot(folder), folderBefore);

        const OpResult re = run(replayJob(Job::Redo, u.record));
        QVERIFY(re.errors.isEmpty());
        QCOMPARE(re.created, (QStringList{a2, folder2}));
        QVERIFY(names(d(QStringLiteral("src"))).isEmpty());
    }

    void moveWithinSameFolderIsNoop()
    {
        const QString a = file(QStringLiteral("a.txt"), "a");
        Job j = makeJob(Job::Move, {a}, m_dir + QLatin1Char('/')); // trailing slash: still the same folder
        j.conflicts.insert(a, Conflict::Replace);
        const OpResult r = run(j);
        QVERIFY(r.errors.isEmpty());
        QVERIFY(r.record.isEmpty());
        QVERIFY(r.created.isEmpty());
        QCOMPARE(names(m_dir), QStringList{QStringLiteral("a.txt")});
        QCOMPARE(readFile(a), QByteArray("a"));
    }

    void moveConflictSkipAndKeepBoth()
    {
        const QString a = file(QStringLiteral("src/a.txt"), "new a");
        const QString b = file(QStringLiteral("src/b.txt"), "new b");
        file(QStringLiteral("dst/a.txt"), "old a");
        file(QStringLiteral("dst/b.txt"), "old b");
        Job j = makeJob(Job::Move, {a, b}, d(QStringLiteral("dst")));
        j.conflicts.insert(a, Conflict::Skip);
        j.conflicts.insert(b, Conflict::KeepBoth);
        const OpResult r = run(j);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.created, QStringList{d(QStringLiteral("dst/b 복사본.txt"))});
        QCOMPARE(readFile(a), QByteArray("new a")); // skipped: stays where it was
        QVERIFY(!util::exists(b));
        QCOMPARE(readFile(d(QStringLiteral("dst/a.txt"))), QByteArray("old a"));
        QCOMPARE(readFile(d(QStringLiteral("dst/b.txt"))), QByteArray("old b"));
        QCOMPARE(readFile(d(QStringLiteral("dst/b 복사본.txt"))), QByteArray("new b"));
    }

    void moveConflictReplaceAndUndo()
    {
        if (!m_canTrash)
            QSKIP("no trash on this system");
        const QString name = T(QStringLiteral("box"));
        const QString src = mk(QStringLiteral("src/") + name);
        file(QStringLiteral("src/") + name + QStringLiteral("/new.txt"), "new");
        const QString target = mk(QStringLiteral("dst/") + name);
        file(QStringLiteral("dst/") + name + QStringLiteral("/old.txt"), "old");
        Job j = makeJob(Job::Move, {src}, d(QStringLiteral("dst")));
        j.conflicts.insert(src, Conflict::Replace);
        const OpResult r = run(j);
        track(r.record);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.record.steps.size(), 2);
        QCOMPARE(r.record.steps[0].kind, Step::Trash);
        QCOMPARE(r.record.steps[1].kind, Step::Move);
        QCOMPARE(names(target), QStringList{QStringLiteral("new.txt")});
        QVERIFY(!util::exists(src));

        const OpResult u = run(replayJob(Job::Undo, r.record));
        QVERIFY2(u.errors.isEmpty(), qPrintable(u.errors.join(QLatin1Char('\n'))));
        QCOMPARE(names(src), QStringList{QStringLiteral("new.txt")});
        QCOMPARE(readFile(QDir(target).filePath(QStringLiteral("old.txt"))), QByteArray("old"));
    }

    void moveMissingSourceReportsError()
    {
        const QString dst = mk(QStringLiteral("dst"));
        const OpResult r = run(makeJob(Job::Move, {d(QStringLiteral("missing.txt"))}, dst));
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.record.isEmpty());
        QVERIFY(names(dst).isEmpty());
    }

    void moveSymlinkMovesTheLinkOnly()
    {
#ifndef Q_OS_UNIX
        QSKIP("symlinks need privileges on Windows");
#else
        const QString target = mk(QStringLiteral("target"));
        file(QStringLiteral("target/a.txt"), "a");
        const QString link = d(QStringLiteral("src/link"));
        mk(QStringLiteral("src"));
        QVERIFY(QFile::link(target, link));
        const QString dst = mk(QStringLiteral("dst"));
        const OpResult r = run(makeJob(Job::Move, {link}, dst));
        QVERIFY(r.errors.isEmpty());
        QVERIFY(!util::exists(link));
        QVERIFY(QFileInfo(QDir(dst).filePath(QStringLiteral("link"))).isSymLink());
        QCOMPARE(names(target), QStringList{QStringLiteral("a.txt")});
#endif
    }

    void moveFolderAcrossVolumes()
    {
        auto other = otherVolumeDir(m_dir);
        if (!other)
            QSKIP("no writable second volume (set GIFILES_TEST_OTHER_VOLUME to a folder on one)");
        const QString src = mk(QStringLiteral("folder"));
        file(QStringLiteral("folder/a.txt"), "a");
        file(QStringLiteral("folder/sub/b.txt"), "b");
        mk(QStringLiteral("folder/empty"));
        const QString single = file(QStringLiteral("single.txt"), "s");
        const auto before = snapshot(src);

        const OpResult r = run(makeJob(Job::Move, {src, single}, other->path()));
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        const QString moved = QDir(other->path()).filePath(QStringLiteral("folder"));
        QCOMPARE(snapshot(moved), before);
        QCOMPARE(readFile(QDir(other->path()).filePath(QStringLiteral("single.txt"))), QByteArray("s"));
        QVERIFY(!util::exists(src));
        QVERIFY(!util::exists(single));

        const OpResult u = run(replayJob(Job::Undo, r.record));
        QVERIFY2(u.errors.isEmpty(), qPrintable(u.errors.join(QLatin1Char('\n'))));
        QCOMPARE(snapshot(src), before);
        QCOMPARE(readFile(single), QByteArray("s"));
        QVERIFY(names(other->path()).isEmpty());
    }

    void moveReplaceOfFolderHoldingTheSourceIsRefused()
    {
        if (!m_canTrash)
            QSKIP("no trash on this system");
        // dst/<x>/<x> moved (or copied) into dst, replacing dst/<x>: Finder refuses this.
        const QString name = T(QStringLiteral("x"));
        const QString holder = mk(name);
        const QString src = file(name + QLatin1Char('/') + name, "precious");
        for (Job::Type t : {Job::Move, Job::Copy}) {
            Job j = makeJob(t, {src}, m_dir);
            j.conflicts.insert(src, Conflict::Replace);
            const OpResult r = run(j);
            track(r.record);
            QCOMPARE(r.errors.size(), 1);
            QVERIFY(r.record.isEmpty());
            QVERIFY(QFileInfo(holder).isDir());
            QCOMPARE(readFile(src), QByteArray("precious"));
        }
    }

    // ---------------------------------------------------------------- duplicate

    void duplicateFileAndFolder()
    {
        const QString a = file(QStringLiteral("a.txt"), "a");
        const QString folder = mk(QStringLiteral("folder"));
        file(QStringLiteral("folder/in.txt"), "in");
        OpResult r = run(makeJob(Job::Duplicate, {a, folder}));
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.record.label, FileOps::verb(Job::Duplicate));
        QCOMPARE(r.created, (QStringList{d(QStringLiteral("a 복사본.txt")), d(QStringLiteral("folder 복사본"))}));
        QCOMPARE(r.record.steps.size(), 2);
        QCOMPARE(r.record.steps[0].kind, Step::Copy);
        QCOMPARE(readFile(d(QStringLiteral("a 복사본.txt"))), QByteArray("a"));
        QCOMPARE(snapshot(d(QStringLiteral("folder 복사본"))), snapshot(folder));

        // Duplicating a copy doesn't nest the suffix: "a 복사본 2.txt", not "a 복사본 복사본.txt".
        r = run(makeJob(Job::Duplicate, {d(QStringLiteral("a 복사본.txt"))}));
        QCOMPARE(r.created, QStringList{d(QStringLiteral("a 복사본 2.txt"))});
    }

    void duplicateMissingSourceReportsError()
    {
        const OpResult r = run(makeJob(Job::Duplicate, {d(QStringLiteral("missing.txt"))}));
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.record.isEmpty());
        QVERIFY(names(m_dir).isEmpty());
    }

    void undoDuplicateTrashesTheCopy()
    {
        if (!m_canTrash)
            QSKIP("no trash on this system");
        const QString a = file(T(QStringLiteral("a.txt")), "a");
        const OpResult r = run(makeJob(Job::Duplicate, {a}));
        QCOMPARE(r.created.size(), 1);
        m_trashNames << QFileInfo(r.created[0]).fileName();
        const OpResult u = run(replayJob(Job::Undo, r.record));
        QVERIFY(u.errors.isEmpty());
        QVERIFY(!util::exists(r.created[0]));
        QCOMPARE(names(m_dir), QStringList{QFileInfo(a).fileName()});
        // Redo copies it again from the original.
        const OpResult re = run(replayJob(Job::Redo, u.record));
        QVERIFY(re.errors.isEmpty());
        QCOMPARE(re.created, r.created);
        QCOMPARE(readFile(r.created[0]), QByteArray("a"));
    }

    // ---------------------------------------------------------------- trash

    void trashAndUndoRedo()
    {
        if (!m_canTrash)
            QSKIP("no trash on this system");
        const QString a = file(T(QStringLiteral("a.txt")), "a");
        const QString folder = mk(T(QStringLiteral("folder")));
        file(QFileInfo(folder).fileName() + QStringLiteral("/in.txt"), "in");
        const OpResult r = run(makeJob(Job::Trash, {a, folder}));
        track(r.record);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.record.label, FileOps::verb(Job::Trash));
        QCOMPARE(r.record.steps.size(), 2);
        QVERIFY(names(m_dir).isEmpty());
        QCOMPARE(readFile(r.record.steps[0].to), QByteArray("a")); // never deleted, only moved
        QCOMPARE(readFile(QDir(r.record.steps[1].to).filePath(QStringLiteral("in.txt"))), QByteArray("in"));

        const OpResult u = run(replayJob(Job::Undo, r.record));
        QVERIFY(u.errors.isEmpty());
        QCOMPARE(u.created, (QStringList{folder, a}));
        QCOMPARE(readFile(a), QByteArray("a"));
        QCOMPARE(readFile(QDir(folder).filePath(QStringLiteral("in.txt"))), QByteArray("in"));

        const OpResult re = run(replayJob(Job::Redo, u.record));
        track(re.record);
        QVERIFY(re.errors.isEmpty());
        QVERIFY(names(m_dir).isEmpty());
        QCOMPARE(readFile(re.record.steps[0].to), QByteArray("a")); // the new trash location is recorded

        const OpResult u2 = run(replayJob(Job::Undo, re.record));
        QVERIFY(u2.errors.isEmpty());
        QCOMPARE(readFile(a), QByteArray("a"));
    }

    void trashMissingItemReportsError()
    {
        const OpResult r = run(makeJob(Job::Trash, {d(QStringLiteral("missing.txt"))}));
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.record.isEmpty());
    }

    // ---------------------------------------------------------------- undo / redo replay

    void undoTrashStepRestoresFromTrashLocation()
    {
        // A synthetic record: the "trash" is just another folder, so the real trash is not used.
        const QString inTrash = file(QStringLiteral("fake-trash/a.txt"), "a");
        const QString orig = d(QStringLiteral("home/a.txt"));
        mk(QStringLiteral("home"));
        const OpResult u = run(replayJob(Job::Undo, UndoRecord{QStringLiteral("t"), {Step{Step::Trash, orig, inTrash}}}));
        QVERIFY(u.errors.isEmpty());
        QCOMPARE(u.created, QStringList{orig});
        QCOMPARE(readFile(orig), QByteArray("a"));
        QVERIFY(!util::exists(inTrash));
    }

    void undoReportsMissingItemsAndGoesOn()
    {
        const QString c = d(QStringLiteral("c.txt"));
        const QString dd = file(QStringLiteral("d.txt"), "d");
        const UndoRecord rec{QStringLiteral("이동"),
                             {Step{Step::Move, d(QStringLiteral("a.txt")), d(QStringLiteral("gone.txt"))}, Step{Step::Move, c, dd}}};
        const OpResult u = run(replayJob(Job::Undo, rec));
        QCOMPARE(u.errors.size(), 1);
        QCOMPARE(u.created, QStringList{c});
        QCOMPARE(readFile(c), QByteArray("d"));
        QVERIFY(!util::exists(d(QStringLiteral("a.txt"))));
    }

    void undoMoveOntoExistingFileFailsSafely()
    {
        const QString from = file(QStringLiteral("a.txt"), "new file made later");
        const QString to = file(QStringLiteral("moved/a.txt"), "moved");
        const OpResult u = run(replayJob(Job::Undo, UndoRecord{QStringLiteral("이동"), {Step{Step::Move, from, to}}}));
        QCOMPARE(u.errors.size(), 1);
        QCOMPARE(readFile(from), QByteArray("new file made later"));
        QCOMPARE(readFile(to), QByteArray("moved"));
    }

    void undoMoveOntoExistingFolderKeepsIt()
    {
        const QString from = mk(QStringLiteral("orig"));
        file(QStringLiteral("orig/made-later.txt"), "user data");
        const QString to = mk(QStringLiteral("moved"));
        file(QStringLiteral("moved/inner.txt"), "moved data");
        const OpResult u = run(replayJob(Job::Undo, UndoRecord{QStringLiteral("이동"), {Step{Step::Move, from, to}}}));
        QCOMPARE(u.errors.size(), 1);
        QCOMPARE(readFile(QDir(from).filePath(QStringLiteral("made-later.txt"))), QByteArray("user data"));
        QCOMPARE(readFile(QDir(to).filePath(QStringLiteral("inner.txt"))), QByteArray("moved data"));
    }

    void undoTrashOntoExistingFolderKeepsIt()
    {
        const QString inTrash = mk(QStringLiteral("fake-trash/box"));
        file(QStringLiteral("fake-trash/box/old.txt"), "trashed data");
        const QString orig = mk(QStringLiteral("box"));
        file(QStringLiteral("box/new.txt"), "user data");
        const OpResult u = run(replayJob(Job::Undo, UndoRecord{QStringLiteral("t"), {Step{Step::Trash, orig, inTrash}}}));
        QCOMPARE(u.errors.size(), 1);
        QCOMPARE(readFile(QDir(orig).filePath(QStringLiteral("new.txt"))), QByteArray("user data"));
        QCOMPARE(readFile(QDir(inTrash).filePath(QStringLiteral("old.txt"))), QByteArray("trashed data"));
    }

    void redoCopyStepCopiesAgain()
    {
        const QString src = mk(QStringLiteral("src"));
        file(QStringLiteral("src/a.txt"), "a");
        const QString to = d(QStringLiteral("copy"));
        const UndoRecord rec{QStringLiteral("복사"), {Step{Step::Copy, src, to}}};
        OpResult re = run(replayJob(Job::Redo, rec));
        QVERIFY(re.errors.isEmpty());
        QCOMPARE(re.created, QStringList{to});
        QCOMPARE(snapshot(to), snapshot(src));
        // A second redo can't copy over the existing copy and says so.
        re = run(replayJob(Job::Redo, rec));
        QCOMPARE(re.errors.size(), 1);
        QCOMPARE(snapshot(to), snapshot(src));
    }

    void undoMkdirRemovesEmptyFolderAndRedoMakesIt()
    {
        QString created;
        UndoRecord rec;
        QVERIFY(FileOps::makeFolder(m_dir, QStringLiteral("new"), &created, &rec).isEmpty());
        const OpResult u = run(replayJob(Job::Undo, rec));
        QVERIFY(u.errors.isEmpty());
        QVERIFY(!util::exists(created)); // empty: removed outright, nothing to keep in the trash
        OpResult re = run(replayJob(Job::Redo, rec));
        QVERIFY(re.errors.isEmpty());
        QCOMPARE(re.created, QStringList{created});
        QVERIFY(QFileInfo(created).isDir());
        re = run(replayJob(Job::Redo, rec));
        QCOMPARE(re.errors.size(), 1); // already there
    }

    void undoMkdirOfFilledFolderTrashesIt()
    {
        if (!m_canTrash)
            QSKIP("no trash on this system");
        QString created;
        UndoRecord rec;
        QVERIFY(FileOps::makeFolder(m_dir, T(QStringLiteral("new")), &created, &rec).isEmpty());
        writeFile(QDir(created).filePath(QStringLiteral("work.txt")), "w");
        m_trashNames << QFileInfo(created).fileName();
        const OpResult u = run(replayJob(Job::Undo, rec));
        QVERIFY(u.errors.isEmpty());
        QVERIFY(!util::exists(created)); // went to the trash with its content (cleaned up at the end)
    }

    // ---------------------------------------------------------------- cancellation

    void preCanceledJobsDoNothing()
    {
        const QString a = file(QStringLiteral("src/a.txt"), "a");
        const QString dst = mk(QStringLiteral("dst"));
        const QString tar = file(QStringLiteral("src/pack.tar"), makeTar({{QStringLiteral("x.txt"), "x"}}));
        const auto before = snapshot(m_dir);
        const UndoRecord rec{QStringLiteral("이동"), {Step{Step::Move, a, QDir(dst).filePath(QStringLiteral("a.txt"))}}};
        for (Job::Type t : {Job::Copy, Job::Move, Job::Duplicate, Job::Trash, Job::Extract, Job::Undo, Job::Redo}) {
            Job j = (t == Job::Undo || t == Job::Redo) ? replayJob(t, rec) : makeJob(t, {t == Job::Extract ? tar : a}, dst);
            j.cancel->store(true);
            const OpResult r = run(j);
            QVERIFY2(r.canceled, qPrintable(FileOps::verb(t)));
            QVERIFY(r.errors.isEmpty());
            if (t != Job::Undo && t != Job::Redo) // see canceledUndoReportsOnlyWhatWasDone
                QVERIFY(r.record.isEmpty());
            QVERIFY(r.created.isEmpty());
            QCOMPARE(snapshot(m_dir), before);
        }
    }

    void canceledUndoReportsOnlyWhatWasDone()
    {
        const QString a = file(QStringLiteral("src/a.txt"), "a");
        const QString dst = mk(QStringLiteral("dst"));
        const UndoRecord rec{QStringLiteral("이동"), {Step{Step::Move, a, QDir(dst).filePath(QStringLiteral("a.txt"))}}};
        for (Job::Type t : {Job::Undo, Job::Redo}) {
            Job j = replayJob(t, rec);
            j.cancel->store(true);
            const OpResult r = run(j);
            QVERIFY(r.canceled);
            QVERIFY2(r.record.isEmpty(), "nothing was replayed, so nothing may move to the other stack");
        }
    }

    void cancelMidCopyDropsPartialFolder()
    {
#ifndef Q_OS_UNIX
        QSKIP("uses a FIFO to hold the copy at a known item");
#else
        // The copy blocks on the FIFO (b.fifo) until the test writes to it; the cancel set
        // meanwhile is seen before c.txt, and the half-made folder must disappear.
        const QString src = mk(QStringLiteral("src/big"));
        file(QStringLiteral("src/big/a.txt"), "a");
        const QString fifo = d(QStringLiteral("src/big/b.fifo"));
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        file(QStringLiteral("src/big/c.txt"), "c");
        const QString dst = mk(QStringLiteral("dst"));
        Job j = makeJob(Job::Copy, {src}, dst);
        QFuture<OpResult> f = FileOps::start(j);
        ScopeExit unblock{[&] { // never leave the worker stuck on the FIFO
            QElapsedTimer t;
            t.start();
            while (!f.isFinished() && t.elapsed() < 10000) {
                const int fd = ::open(QFile::encodeName(fifo).constData(), O_RDWR | O_NONBLOCK);
                if (fd >= 0) {
                    (void)!::write(fd, "x", 1);
                    ::close(fd);
                }
                QThread::msleep(5);
            }
            f.waitForFinished();
        }};
        QTRY_COMPARE_WITH_TIMEOUT(f.progressText(), QStringLiteral("b.fifo"), 10000);
        j.cancel->store(true);
        unblock.fn();
        const OpResult r = f.result();
        QVERIFY(r.canceled);
        QVERIFY(r.errors.isEmpty());
        QVERIFY(r.record.isEmpty());
        QVERIFY(r.created.isEmpty());
        QVERIFY2(names(dst).isEmpty(), "the partial copy must be removed");
        QCOMPARE(names(src).size(), 3);
#endif
    }

    void cancelMidExtractStopsTheTool()
    {
#ifndef Q_OS_UNIX
        QSKIP("uses a FIFO as a never-ending archive");
#else
        if (!haveExtractor(false))
            QSKIP("tar not found");
        const QString fifo = d(QStringLiteral("stuck.tar"));
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        Job j = makeJob(Job::Extract, {fifo});
        QFuture<OpResult> f = FileOps::start(j);
        QTRY_COMPARE_WITH_TIMEOUT(f.progressText(), QStringLiteral("stuck.tar"), 10000);
        j.cancel->store(true);
        QTRY_VERIFY_WITH_TIMEOUT(f.isFinished(), 10000);
        const OpResult r = f.result();
        QVERIFY(r.canceled);
        QVERIFY(r.errors.isEmpty());
        QVERIFY(r.record.isEmpty());
        QCOMPARE(names(m_dir), QStringList{QStringLiteral("stuck.tar")}); // no temporary folder left
#endif
    }

    // ---------------------------------------------------------------- rename / new folder

    void renameFileAndFolderAndUndo()
    {
        const QString a = file(QStringLiteral("a.txt"), "a");
        const QString folder = mk(QStringLiteral("folder"));
        file(QStringLiteral("folder/in.txt"), "in");
        UndoRecord rec;
        QCOMPARE(FileOps::rename(a, QStringLiteral("b.txt"), &rec), QString());
        QVERIFY(!rec.label.isEmpty());
        QCOMPARE(rec.steps.size(), 1);
        QCOMPARE(rec.steps[0].kind, Step::Move);
        QCOMPARE(rec.steps[0].from, a);
        QCOMPARE(rec.steps[0].to, d(QStringLiteral("b.txt")));
        QCOMPARE(readFile(d(QStringLiteral("b.txt"))), QByteArray("a"));
        QVERIFY(!util::exists(a));
        QVERIFY(run(replayJob(Job::Undo, rec)).errors.isEmpty());
        QCOMPARE(readFile(a), QByteArray("a"));

        UndoRecord rec2;
        QCOMPARE(FileOps::rename(folder, QStringLiteral("renamed"), &rec2), QString());
        QCOMPARE(readFile(d(QStringLiteral("renamed/in.txt"))), QByteArray("in"));
        QVERIFY(run(replayJob(Job::Undo, rec2)).errors.isEmpty());
        QCOMPARE(readFile(d(QStringLiteral("folder/in.txt"))), QByteArray("in"));
    }

    void renameRefusesBadOrTakenNames()
    {
        const QString a = file(QStringLiteral("a.txt"), "a");
        file(QStringLiteral("b.txt"), "b");
        UndoRecord rec;
        for (const QString &bad : {QString(), QStringLiteral("   "), QStringLiteral("."), QStringLiteral(".."), QStringLiteral("x/y")})
            QVERIFY2(!FileOps::rename(a, bad, &rec).isEmpty(), qPrintable(bad));
        QVERIFY(!FileOps::rename(a, QStringLiteral("b.txt"), &rec).isEmpty());
        QVERIFY(rec.isEmpty());
        QVERIFY(rec.label.isEmpty());
        QCOMPARE(readFile(a), QByteArray("a"));
        QCOMPARE(readFile(d(QStringLiteral("b.txt"))), QByteArray("b"));

        // Same name: nothing to do, nothing recorded.
        QCOMPARE(FileOps::rename(a, QStringLiteral("a.txt"), &rec), QString());
        QVERIFY(rec.isEmpty());
        // Missing item: the file system refuses.
        QVERIFY(!FileOps::rename(d(QStringLiteral("missing.txt")), QStringLiteral("c.txt"), &rec).isEmpty());
        QVERIFY(rec.isEmpty());
    }

    void renameCaseOnly()
    {
        const QString a = file(QStringLiteral("a.txt"), "a");
        UndoRecord rec;
        QCOMPARE(FileOps::rename(a, QStringLiteral("A.txt"), &rec), QString());
        QCOMPARE(names(m_dir), QStringList{QStringLiteral("A.txt")});
        QCOMPARE(rec.steps.size(), 1);
    }

    void makeFolderPicksFreeNames()
    {
        QString c1, c2;
        UndoRecord r1, r2;
        QCOMPARE(FileOps::makeFolder(m_dir, QStringLiteral("새로운 폴더"), &c1, &r1), QString());
        QCOMPARE(FileOps::makeFolder(m_dir, QStringLiteral("새로운 폴더"), &c2, &r2), QString());
        QCOMPARE(c1, d(QStringLiteral("새로운 폴더")));
        QCOMPARE(c2, d(QStringLiteral("새로운 폴더 2")));
        QVERIFY(QFileInfo(c2).isDir());
        QCOMPARE(r2.steps.size(), 1);
        QCOMPARE(r2.steps[0].kind, Step::Mkdir);
        QCOMPARE(r2.steps[0].to, c2);

        QString c3 = QStringLiteral("untouched");
        UndoRecord r3;
        QVERIFY(!FileOps::makeFolder(d(QStringLiteral("missing/deeper")), QStringLiteral("x"), &c3, &r3).isEmpty());
        QCOMPARE(c3, QStringLiteral("untouched"));
        QVERIFY(r3.isEmpty());
    }

    void makeFolderWithMovesItemsAndUndo()
    {
        const QString a = file(QStringLiteral("a.txt"), "a");
        const QString folder = mk(QStringLiteral("folder"));
        file(QStringLiteral("folder/in.txt"), "in");
        const QString stays = file(QStringLiteral("stays.txt"), "s");
        QString created;
        UndoRecord rec;
        QCOMPARE(FileOps::makeFolderWith(m_dir, {a, folder}, &created, &rec), QString());
        QVERIFY(QFileInfo(created).isDir());
        QCOMPARE(QFileInfo(created).absolutePath(), QFileInfo(m_dir).absoluteFilePath());
        QCOMPARE(names(created), (QStringList{QStringLiteral("a.txt"), QStringLiteral("folder")}));
        QCOMPARE(rec.steps.size(), 3);
        QCOMPARE(rec.steps[0].kind, Step::Mkdir);
        QCOMPARE(rec.steps[1].kind, Step::Move);
        QCOMPARE(rec.steps[2].kind, Step::Move);
        QCOMPARE(readFile(stays), QByteArray("s"));

        // Undo runs backwards: the items come out first, then the now empty folder goes away.
        const OpResult u = run(replayJob(Job::Undo, rec));
        QVERIFY(u.errors.isEmpty());
        QVERIFY(!util::exists(created));
        QCOMPARE(readFile(a), QByteArray("a"));
        QCOMPARE(readFile(d(QStringLiteral("folder/in.txt"))), QByteArray("in"));
    }

    void makeFolderWithStopsAtFirstFailure()
    {
        const QString a = file(QStringLiteral("a.txt"), "a");
        const QString c = file(QStringLiteral("c.txt"), "c");
        QString created;
        UndoRecord rec;
        const QString err = FileOps::makeFolderWith(m_dir, {a, d(QStringLiteral("missing.txt")), c}, &created, &rec);
        QVERIFY(err.contains(QStringLiteral("missing.txt")));
        // What was done is recorded so it can be undone; the rest is untouched.
        QCOMPARE(rec.steps.size(), 2);
        QCOMPARE(names(created), QStringList{QStringLiteral("a.txt")});
        QCOMPARE(readFile(c), QByteArray("c"));
        QVERIFY(run(replayJob(Job::Undo, rec)).errors.isEmpty());
        QVERIFY(!util::exists(created));
        QCOMPARE(readFile(a), QByteArray("a"));

        QString none;
        UndoRecord rec2;
        QVERIFY(!FileOps::makeFolderWith(d(QStringLiteral("missing")), {c}, &none, &rec2).isEmpty());
        QVERIFY(rec2.isEmpty());
        QCOMPARE(readFile(c), QByteArray("c"));
    }

    // ---------------------------------------------------------------- extract

    void extractSingleItemComesOutAsIs()
    {
        if (!haveExtractor(false))
            QSKIP("tar not found");
        const QString inner = T(QStringLiteral("one.txt"));
        const QString tar = file(QStringLiteral("one.tar"), makeTar({{inner, "content"}}));
        OpResult r = run(makeJob(Job::Extract, {tar}));
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        const QString out = d(inner);
        QCOMPARE(r.created, QStringList{out});
        QCOMPARE(r.record.label, FileOps::verb(Job::Extract));
        QCOMPARE(r.record.steps.size(), 1);
        QCOMPARE(r.record.steps[0].kind, Step::Extract);
        QCOMPARE(r.record.steps[0].from, tar);
        QCOMPARE(r.record.steps[0].to, out);
        QCOMPARE(readFile(out), QByteArray("content"));
        QVERIFY(QFileInfo(tar).isFile()); // the archive stays
        QVERIFY(leftovers(m_dir).isEmpty());

        // Again: the name is taken, so "one 2.txt".
        const OpResult r2 = run(makeJob(Job::Extract, {tar}));
        QString base, ext;
        util::splitExt(inner, &base, &ext);
        const QString out2 = d(base + QStringLiteral(" 2") + ext);
        QCOMPARE(r2.created, QStringList{out2});
        QCOMPARE(readFile(out2), QByteArray("content"));

        if (!m_canTrash)
            return;
        // Undo moves the result to the trash; redo extracts again and records the new result.
        m_trashNames << QFileInfo(out2).fileName();
        const OpResult u = run(replayJob(Job::Undo, r2.record));
        QVERIFY(u.errors.isEmpty());
        QVERIFY(!util::exists(out2));
        const OpResult re = run(replayJob(Job::Redo, u.record));
        QVERIFY(re.errors.isEmpty());
        QCOMPARE(re.created, QStringList{out2});
        QCOMPARE(re.record.steps[0].to, out2);
        QCOMPARE(readFile(out2), QByteArray("content"));
        QVERIFY(leftovers(m_dir).isEmpty());
    }

    void extractSeveralItemsGoIntoFolderNamedAfterArchive()
    {
        if (!haveExtractor(false))
            QSKIP("tar not found");
        const QByteArray data = makeTar({{QStringLiteral("a.txt"), "A"},
                                         {QStringLiteral("sub"), {}, true},
                                         {QStringLiteral("sub/b.txt"), "B"}});
        const QString tar = file(QStringLiteral("pack.tar"), data);
        OpResult r = run(makeJob(Job::Extract, {tar}));
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        QCOMPARE(r.created, QStringList{d(QStringLiteral("pack"))});
        QCOMPARE(readFile(d(QStringLiteral("pack/a.txt"))), QByteArray("A"));
        QCOMPARE(readFile(d(QStringLiteral("pack/sub/b.txt"))), QByteArray("B"));

        r = run(makeJob(Job::Extract, {tar}));
        QCOMPARE(r.created, QStringList{d(QStringLiteral("pack 2"))});

        // Not a known archive suffix: the folder is named after the file without its extension.
        const QString odd = file(QStringLiteral("bundle.dat"), data);
        r = run(makeJob(Job::Extract, {odd}));
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        QCOMPARE(r.created, QStringList{d(QStringLiteral("bundle"))});
        QVERIFY(leftovers(m_dir).isEmpty());
    }

    void extractZip()
    {
        if (!haveExtractor(true))
            QSKIP("no zip extractor (ditto / unzip / tar.exe)");
        const QString zip = file(QStringLiteral("pack.zip"),
                                 makeZip({{QStringLiteral("a.txt"), "A"},
                                          {QStringLiteral("dir"), {}, true},
                                          {QStringLiteral("dir/b.txt"), "B"},
                                          {QStringLiteral("__MACOSX"), {}, true},
                                          {QStringLiteral("__MACOSX/junk.txt"), "J"}}));
        OpResult r = run(makeJob(Job::Extract, {zip}));
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        QCOMPARE(r.created, QStringList{d(QStringLiteral("pack"))});
        QCOMPARE(names(d(QStringLiteral("pack"))), (QStringList{QStringLiteral("a.txt"), QStringLiteral("dir")}));
        QCOMPARE(readFile(d(QStringLiteral("pack/dir/b.txt"))), QByteArray("B"));

        // A single item next to __MACOSX still comes out as is.
        const QString one = file(QStringLiteral("one.zip"),
                                 makeZip({{QStringLiteral("only.txt"), "O"}, {QStringLiteral("__MACOSX/notes.txt"), "J"}}));
        r = run(makeJob(Job::Extract, {one}));
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        QCOMPARE(r.created, QStringList{d(QStringLiteral("only.txt"))});
        QCOMPARE(readFile(d(QStringLiteral("only.txt"))), QByteArray("O"));
        QVERIFY(!util::exists(d(QStringLiteral("__MACOSX"))));
        QVERIFY(leftovers(m_dir).isEmpty());
    }

    void extractBrokenArchivesFailCleanly()
    {
        if (!haveExtractor(false))
            QSKIP("tar not found");
        const QString good = file(QStringLiteral("good.tar"), makeTar({{QStringLiteral("good.txt"), "g"}}));
        const QString bad = file(QStringLiteral("bad.tar"), QByteArray("this is not an archive at all").repeated(40));
        const QString empty = file(QStringLiteral("empty.tar"), makeTar({}));
        QStringList sources{bad, good, empty};
        if (haveExtractor(true))
            sources << file(QStringLiteral("bad.zip"), QByteArray("PK\x03\x04 broken"));
        const QStringList before = names(m_dir);
        const OpResult r = run(makeJob(Job::Extract, sources));
        QCOMPARE(r.errors.size(), sources.size() - 1);
        for (const QString &e : r.errors)
            QVERIFY(!e.isEmpty());
        QCOMPARE(r.created, QStringList{d(QStringLiteral("good.txt"))}); // one failure doesn't stop the rest
        QCOMPARE(r.record.steps.size(), 1);
        QVERIFY2(leftovers(m_dir).isEmpty(), "temporary folders must be removed");
        QCOMPARE(names(m_dir), QStringList(before) << QStringLiteral("good.txt"));
    }

    void extractIntoReadOnlyFolderFails()
    {
        if (!posixPermsEnforced())
            QSKIP("needs enforced POSIX permissions (not Windows, not root)");
        const QString tar = file(QStringLiteral("ro/one.tar"), makeTar({{QStringLiteral("a.txt"), "a"}}));
        PermGuard g(d(QStringLiteral("ro")), kReadOnlyDir);
        const OpResult r = run(makeJob(Job::Extract, {tar}));
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.record.isEmpty());
        QCOMPARE(names(d(QStringLiteral("ro"))), QStringList{QStringLiteral("one.tar")});
    }
};

QTEST_GUILESS_MAIN(Unit)
#include "unit_fileops.moc"
