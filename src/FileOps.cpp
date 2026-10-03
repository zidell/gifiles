#include "FileOps.h"
#include "Util.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QPromise>
#include <QRandomGenerator>
#include <QtConcurrent>

namespace {

struct Ctx {
    QPromise<OpResult> &promise;
    OpResult &res;
    const std::atomic<bool> &cancel;
    int done = 0;

    bool canceled()
    {
        if (cancel.load())
            res.canceled = true;
        return res.canceled;
    }
    void tick(const QString &name)
    {
        promise.setProgressValueAndText(++done, name);
    }
};

const QDir::Filters kAll = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;

int countFiles(const QString &path)
{
    QFileInfo fi(path);
    if (!fi.isDir() || fi.isSymLink())
        return 1;
    int n = 1;
    QDirIterator it(path, kAll, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++n;
    }
    return n;
}

bool copyRecursive(const QString &src, const QString &dst, Ctx &ctx, QString *err)
{
    if (ctx.canceled())
        return false;
    QFileInfo fi(src);
    ctx.tick(fi.fileName());
    if (fi.isSymLink()) {
        // The link as written: a relative link must stay relative, or the copy points back into the original.
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        const QString linkTarget = fi.readSymLink();
#else
        const QString linkTarget = fi.symLinkTarget();
#endif
        if (!QFile::link(linkTarget, dst)) {
            *err = Gifiles::tr("링크를 만들 수 없습니다: %1").arg(fi.fileName());
            return false;
        }
        return true;
    }
    if (fi.isDir()) {
        if (!QDir(src).isReadable()) { // listing it would look empty and the copy would miss its contents
            *err = Gifiles::tr("폴더를 읽을 수 없습니다: %1").arg(fi.fileName());
            return false;
        }
        if (!QDir().mkdir(dst)) {
            *err = Gifiles::tr("폴더를 만들 수 없습니다: %1").arg(dst);
            return false;
        }
        const auto entries = QDir(src).entryInfoList(kAll);
        for (const QFileInfo &e : entries)
            if (!copyRecursive(e.filePath(), QDir(dst).filePath(e.fileName()), ctx, err))
                return false;
        QFile::setPermissions(dst, fi.permissions());
        return true;
    }
    QFile f(src);
    if (!f.copy(dst)) {
        *err = Gifiles::tr("%1 복사 실패: %2").arg(fi.fileName(), f.errorString());
        return false;
    }
    return true;
}

bool moveItem(const QString &src, const QString &dst, Ctx &ctx, QString *err)
{
    QFileInfo fi(src);
    // Never onto an existing item: a folder rename would fail and the cross-volume fallback below
    // would then remove what is there (undo after a new item took the old name).
    if (util::exists(dst)) {
        *err = Gifiles::tr("\"%1\"이(가) 이미 있어 옮기지 않았습니다.").arg(QDir::toNativeSeparators(dst));
        return false;
    }
    if (fi.isDir() && !fi.isSymLink()) {
        if (QDir().rename(src, dst))
            return true;
        // Different volume: copy then delete the original.
        if (!copyRecursive(src, dst, ctx, err)) {
            QDir(dst).removeRecursively();
            return false;
        }
        if (!QDir(src).removeRecursively()) {
            *err = Gifiles::tr("원본을 지우지 못했습니다: %1").arg(src);
            return false;
        }
        return true;
    }
    QFile f(src);
    if (!f.rename(dst)) {
        *err = Gifiles::tr("%1 이동 실패: %2").arg(fi.fileName(), f.errorString());
        return false;
    }
    return true;
}

bool trashItem(const QString &path, QString *inTrash, QString *err)
{
    QFile f(path);
    if (!f.moveToTrash()) {
        *err = Gifiles::tr("휴지통으로 옮기지 못했습니다: %1 (%2)").arg(QFileInfo(path).fileName(), f.errorString());
        return false;
    }
    // Across file systems Qt copies into the trash and then removes the original; when that removal
    // fails (a read-only folder) it still reports success. The item hasn't moved: say so.
    if (util::exists(path)) {
        *err = Gifiles::tr("휴지통으로 옮기지 못했습니다: %1 (%2)").arg(QFileInfo(path).fileName(), Gifiles::tr("원본을 지울 수 없습니다"));
        return false;
    }
    *inTrash = f.fileName(); // the item's new location inside the trash
    return true;
}

const char *const kArchiveSuffixes[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".zip", ".tar", ".tgz",
                                        ".tbz", ".tbz2", ".txz", ".7z", ".rar", ".xar", ".cpio"};

QString archiveBaseName(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    for (const char *suf : kArchiveSuffixes)
        if (name.endsWith(QLatin1String(suf), Qt::CaseInsensitive) && name.size() > int(strlen(suf)))
            return name.left(name.size() - int(strlen(suf)));
    return QFileInfo(path).completeBaseName();
}

// Runs an extraction tool to completion, killing it if the job is canceled.
bool runTool(const QString &program, const QStringList &args, Ctx &ctx, QString *err)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, args);
    if (!p.waitForStarted()) {
        *err = Gifiles::tr("%1을(를) 실행할 수 없습니다.").arg(program);
        return false;
    }
    while (p.state() != QProcess::NotRunning && !p.waitForFinished(100)) {
        if (ctx.canceled()) {
            p.kill();
            p.waitForFinished();
            return false;
        }
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        const QString msg = QString::fromUtf8(p.readAllStandardError()).trimmed();
        *err = msg.isEmpty() ? Gifiles::tr("압축을 풀 수 없습니다.") : msg.section(QLatin1Char('\n'), 0, 0);
        return false;
    }
    return true;
}

// Expands an archive into dir the way Archive Utility does: a single top-level item comes out
// as is, several items go into a folder named after the archive. Names never clash.
bool extractArchive(const QString &archive, const QString &dir, Ctx &ctx, QString *result, QString *err)
{
    const QString tmp = QDir(dir).filePath(QStringLiteral(".gifiles-extract-%1").arg(QRandomGenerator::global()->generate(), 0, 16));
    if (!QDir().mkdir(tmp)) {
        *err = Gifiles::tr("이 위치에 압축을 풀 수 없습니다.");
        return false;
    }
    const bool zip = archive.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive);
    bool ok;
#if defined(Q_OS_MACOS)
    ok = zip ? runTool(QStringLiteral("/usr/bin/ditto"), {QStringLiteral("-x"), QStringLiteral("-k"), QStringLiteral("--sequesterRsrc"), QStringLiteral("--rsrc"), archive, tmp}, ctx, err)
             : runTool(QStringLiteral("/usr/bin/tar"), {QStringLiteral("-xf"), archive, QStringLiteral("-C"), tmp}, ctx, err);
#elif defined(Q_OS_WIN)
    ok = runTool(QStringLiteral("tar.exe"), {QStringLiteral("-xf"), archive, QStringLiteral("-C"), tmp}, ctx, err);
#else
    ok = zip ? runTool(QStringLiteral("unzip"), {QStringLiteral("-q"), archive, QStringLiteral("-d"), tmp}, ctx, err)
             : runTool(QStringLiteral("tar"), {QStringLiteral("-xf"), archive, QStringLiteral("-C"), tmp}, ctx, err);
#endif
    if (!ok) {
        QDir(tmp).removeRecursively();
        if (err->isEmpty())
            *err = Gifiles::tr("압축을 풀 수 없습니다: %1").arg(QFileInfo(archive).fileName());
        return false;
    }
    QDir(QDir(tmp).filePath(QStringLiteral("__MACOSX"))).removeRecursively();
    const QStringList entries = QDir(tmp).entryList(kAll);
    if (entries.isEmpty()) {
        QDir(tmp).removeRecursively();
        *err = Gifiles::tr("압축 파일이 비어 있습니다: %1").arg(QFileInfo(archive).fileName());
        return false;
    }
    if (entries.size() == 1) {
        *result = QDir(dir).filePath(util::uniquePlainName(dir, entries.first()));
        ok = QDir().rename(QDir(tmp).filePath(entries.first()), *result);
        QDir().rmdir(tmp);
    } else {
        *result = QDir(dir).filePath(util::uniquePlainName(dir, archiveBaseName(archive)));
        ok = QDir().rename(tmp, *result);
    }
    if (!ok) {
        QDir(tmp).removeRecursively();
        *err = Gifiles::tr("압축을 푼 항목을 옮기지 못했습니다.");
    }
    return ok;
}

void extract(const Job &job, Ctx &ctx)
{
    ctx.res.record.label = Gifiles::tr("압축 풀기");
    for (const QString &archive : job.sources) {
        if (ctx.canceled())
            return;
        ctx.tick(QFileInfo(archive).fileName());
        QString result, err;
        if (!extractArchive(archive, QFileInfo(archive).absolutePath(), ctx, &result, &err)) {
            if (!ctx.res.canceled)
                ctx.res.errors << err;
            continue;
        }
        ctx.res.record.steps << Step{Step::Extract, archive, result};
        ctx.res.created << result;
    }
}

// Copy or move sources into destDir, honoring per-item conflict decisions.
void transfer(const Job &job, Ctx &ctx, bool move)
{
    ctx.res.record.label = move ? Gifiles::tr("이동") : Gifiles::tr("복사");
    for (const QString &src : job.sources) {
        if (ctx.canceled())
            return;
        QFileInfo fi(src);
        const QString name = fi.fileName();
        const bool sameDir = QDir::cleanPath(fi.absolutePath()) == QDir::cleanPath(job.destDir);
        if (move && sameDir)
            continue;
        if (fi.isDir() && util::isInside(job.destDir, src)) {
            ctx.res.errors << Gifiles::tr("\"%1\"을(를) 자기 자신 안으로 옮길 수 없습니다.").arg(name);
            continue;
        }
        QString target = QDir(job.destDir).filePath(name);
        if (util::exists(target)) {
            Conflict c = sameDir ? Conflict::KeepBoth : job.conflicts.value(src, Conflict::KeepBoth);
            if (c == Conflict::Skip)
                continue;
            if (c == Conflict::KeepBoth) {
                target = QDir(job.destDir).filePath(util::uniqueCopyName(job.destDir, name, false));
            } else if (util::isInside(src, target)) { // replacing it would trash the source too
                ctx.res.errors << Gifiles::tr("\"%1\"은(는) 옮길 항목을 담고 있어 바꿀 수 없습니다.").arg(name);
                continue;
            } else {
                QString inTrash, err;
                if (!trashItem(target, &inTrash, &err)) {
                    ctx.res.errors << err;
                    continue;
                }
                ctx.res.record.steps << Step{Step::Trash, target, inTrash};
            }
        }
        QString err;
        bool ok = move ? moveItem(src, target, ctx, &err) : copyRecursive(src, target, ctx, &err);
        if (!ok) {
            if (!ctx.res.canceled)
                ctx.res.errors << err;
            if (!move) { // drop the partial copy
                if (QFileInfo(target).isDir())
                    QDir(target).removeRecursively();
                else
                    QFile::remove(target);
            }
            continue;
        }
        if (move)
            ctx.tick(name);
        ctx.res.record.steps << Step{move ? Step::Move : Step::Copy, src, target};
        ctx.res.created << target;
    }
}

void duplicate(const Job &job, Ctx &ctx)
{
    ctx.res.record.label = Gifiles::tr("복제");
    for (const QString &src : job.sources) {
        if (ctx.canceled())
            return;
        QFileInfo fi(src);
        const QString dir = fi.absolutePath();
        const QString target = QDir(dir).filePath(util::uniqueCopyName(dir, fi.fileName(), true));
        QString err;
        if (!copyRecursive(src, target, ctx, &err)) {
            if (!ctx.res.canceled)
                ctx.res.errors << err;
            continue;
        }
        ctx.res.record.steps << Step{Step::Copy, src, target};
        ctx.res.created << target;
    }
}

void trash(const Job &job, Ctx &ctx)
{
    ctx.res.record.label = Gifiles::tr("휴지통으로 이동");
    for (const QString &src : job.sources) {
        if (ctx.canceled())
            return;
        ctx.tick(QFileInfo(src).fileName());
        QString inTrash, err;
        if (!trashItem(src, &inTrash, &err)) {
            ctx.res.errors << err;
            continue;
        }
        ctx.res.record.steps << Step{Step::Trash, src, inTrash};
    }
}

// Replays a record forwards (redo) or backwards (undo). Steps that move things into the trash
// get their new trash location written back so the record stays reversible.
void replay(const Job &job, Ctx &ctx, bool inverse)
{
    UndoRecord rec = job.record;
    const int n = rec.steps.size();
    QList<bool> replayed(n, false);
    for (int k = 0; k < n; ++k) {
        if (ctx.canceled())
            break;
        replayed[inverse ? n - 1 - k : k] = true;
        Step &s = rec.steps[inverse ? n - 1 - k : k];
        QString err;
        bool ok = true;
        switch (s.kind) {
        case Step::Move:
            ok = inverse ? moveItem(s.to, s.from, ctx, &err) : moveItem(s.from, s.to, ctx, &err);
            if (ok)
                ctx.res.created << (inverse ? s.from : s.to);
            break;
        case Step::Copy:
            if (inverse) {
                QString t;
                ok = trashItem(s.to, &t, &err);
            } else {
                ok = copyRecursive(s.from, s.to, ctx, &err);
                if (ok)
                    ctx.res.created << s.to;
            }
            break;
        case Step::Trash:
            if (inverse) {
                ok = moveItem(s.to, s.from, ctx, &err);
                if (ok)
                    ctx.res.created << s.from;
            } else {
                ok = trashItem(s.from, &s.to, &err);
            }
            break;
        case Step::Mkdir:
            if (inverse) {
                if (!QDir().rmdir(s.to)) {
                    QString t;
                    ok = trashItem(s.to, &t, &err);
                }
            } else {
                ok = QDir().mkdir(s.to);
                if (ok)
                    ctx.res.created << s.to;
                else
                    err = Gifiles::tr("폴더를 만들 수 없습니다: %1").arg(s.to);
            }
            break;
        case Step::Extract:
            if (inverse) {
                QString t;
                ok = trashItem(s.to, &t, &err);
            } else {
                ok = extractArchive(s.from, QFileInfo(s.from).absolutePath(), ctx, &s.to, &err);
                if (ok)
                    ctx.res.created << s.to;
            }
            break;
        }
        if (!ok)
            ctx.res.errors << err;
    }
    // Canceled part-way: only what was replayed moves to the other stack.
    ctx.res.record.label = ctx.res.rest.label = rec.label;
    for (int i = 0; i < n; ++i)
        (replayed[i] ? ctx.res.record : ctx.res.rest).steps << rec.steps[i];
}

} // namespace

namespace FileOps {

QString verb(Job::Type t)
{
    switch (t) {
    case Job::Copy: return Gifiles::tr("복사");
    case Job::Move: return Gifiles::tr("이동");
    case Job::Trash: return Gifiles::tr("휴지통으로 이동");
    case Job::Duplicate: return Gifiles::tr("복제");
    case Job::Extract: return Gifiles::tr("압축 풀기");
    case Job::Undo: return Gifiles::tr("실행 취소");
    case Job::Redo: return Gifiles::tr("다시 실행");
    }
    return {};
}

QFuture<OpResult> start(const Job &job)
{
    return QtConcurrent::run([job](QPromise<OpResult> &promise) {
        OpResult res;
        res.type = job.type;
        res.destDir = job.destDir;
        int total = 0;
        if (job.type == Job::Copy || job.type == Job::Duplicate)
            for (const QString &s : job.sources)
                total += countFiles(s);
        else
            total = qMax<qsizetype>(job.sources.size(), job.record.steps.size());
        promise.setProgressRange(0, qMax(1, total));
        Ctx ctx{promise, res, *job.cancel};
        switch (job.type) {
        case Job::Copy: transfer(job, ctx, false); break;
        case Job::Move: transfer(job, ctx, true); break;
        case Job::Duplicate: duplicate(job, ctx); break;
        case Job::Extract: extract(job, ctx); break;
        case Job::Trash: trash(job, ctx); break;
        case Job::Undo: replay(job, ctx, true); break;
        case Job::Redo: replay(job, ctx, false); break;
        }
        promise.addResult(res);
    });
}

bool isArchive(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.isFile())
        return false;
    const QString name = fi.fileName();
    for (const char *suf : kArchiveSuffixes)
        if (name.endsWith(QLatin1String(suf), Qt::CaseInsensitive))
            return true;
    return false;
}

QString rename(const QString &path, const QString &newName, UndoRecord *rec)
{
    if (QString e = util::validateName(newName); !e.isEmpty())
        return e;
    QFileInfo fi(path);
    if (fi.fileName() == newName)
        return {};
    const QString dst = QDir(fi.absolutePath()).filePath(newName);
    // Case-only renames are fine on case-insensitive file systems.
    if (util::exists(dst) && fi.fileName().compare(newName, Qt::CaseInsensitive) != 0)
        return Gifiles::tr("\"%1\" 이름은 이미 사용 중입니다. 다른 이름을 선택하십시오.").arg(newName);
    bool ok = fi.isDir() ? QDir().rename(path, dst) : QFile::rename(path, dst);
    if (!ok)
        return Gifiles::tr("이름을 바꿀 수 없습니다.");
    rec->label = Gifiles::tr("이름 변경");
    rec->steps = {Step{Step::Move, path, dst}};
    return {};
}

QString makeFolder(const QString &dir, const QString &name, QString *created, UndoRecord *rec)
{
    const QString path = QDir(dir).filePath(util::uniquePlainName(dir, name));
    if (!QDir().mkdir(path))
        return Gifiles::tr("이 위치에 폴더를 만들 수 없습니다.");
    *created = path;
    rec->label = Gifiles::tr("새로운 폴더");
    rec->steps = {Step{Step::Mkdir, {}, path}};
    return {};
}

} // namespace FileOps

namespace FileOps {

QString makeFolderWith(const QString &dir, const QStringList &items, QString *created, UndoRecord *rec)
{
    QString err = makeFolder(dir, Gifiles::tr("항목이 포함된 새로운 폴더"), created, rec);
    if (!err.isEmpty())
        return err;
    rec->label = Gifiles::tr("선택 항목으로 새로운 폴더");
    for (const QString &src : items) {
        const QString dst = QDir(*created).filePath(QFileInfo(src).fileName());
        const bool ok = QFileInfo(src).isDir() ? QDir().rename(src, dst) : QFile::rename(src, dst);
        if (!ok) {
            err = Gifiles::tr("\"%1\"을(를) 옮기지 못했습니다.").arg(QFileInfo(src).fileName());
            break;
        }
        rec->steps << Step{Step::Move, src, dst};
    }
    return err;
}

} // namespace FileOps
