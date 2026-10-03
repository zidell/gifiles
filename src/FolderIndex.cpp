#include "FolderIndex.h"
#include "Util.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>

#include <algorithm>
#include <string_view>

#ifdef Q_OS_WIN
#include <QDirIterator>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {

constexpr quint32 kMagic = 0x47465431; // "GFT1"
constexpr quint32 kFormat = 1;

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

QString fold(const QString &s) { return s.normalized(QString::NormalizationForm_C).toCaseFolded(); }
QString foldPath(const QString &s) { return kPathCase == Qt::CaseInsensitive ? fold(s) : s.normalized(QString::NormalizationForm_C); }

QString join(const QString &dir, const QString &name)
{
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

// "/a/b/" -> "/a/b", but "/" and "C:/" stay.
QString cleanRoot(const QString &path)
{
    QString p = QDir::fromNativeSeparators(path);
    while (p.size() > 1 && p.endsWith(QLatin1Char('/')) && !(p.size() == 3 && p[1] == QLatin1Char(':')))
        p.chop(1);
    if (p.size() == 2 && p[1] == QLatin1Char(':'))
        p += QLatin1Char('/');
    return p;
}

bool isAbsoluteRule(const QString &rule)
{
    return rule.startsWith(QLatin1Char('/')) || rule.startsWith(QLatin1Char('~'))
        || (rule.size() >= 2 && rule[1] == QLatin1Char(':'));
}

struct Rules {
    QSet<QString> names, paths; // folded
    QList<QRegularExpression> nameRx, pathRx;
    bool skipPackages = false;

    explicit Rules(const FolderIndex::Options &o, const QStringList &mounts) : skipPackages(o.skipPackages)
    {
        for (QString rule : o.exclude) {
            rule = rule.trimmed();
            if (rule.isEmpty())
                continue;
            const bool wild = rule.contains(QLatin1Char('*')) || rule.contains(QLatin1Char('?')) || rule.contains(QLatin1Char('['));
            if (isAbsoluteRule(rule)) {
                if (rule.startsWith(QLatin1Char('~')))
                    rule = QDir::homePath() + rule.mid(1);
                rule = cleanRoot(rule);
                if (wild)
                    pathRx << QRegularExpression(QRegularExpression::wildcardToRegularExpression(foldPath(rule)));
                else
                    paths << foldPath(rule);
            } else if (wild) {
                nameRx << QRegularExpression(QRegularExpression::wildcardToRegularExpression(fold(rule)));
            } else {
                names << fold(rule);
            }
        }
        for (const QString &m : mounts)
            paths << foldPath(cleanRoot(m));
    }

    bool excluded(const QString &name, const QString &path) const
    {
        const QString n = fold(name);
        if (names.contains(n))
            return true;
        for (const QRegularExpression &rx : nameRx)
            if (rx.match(n).hasMatch())
                return true;
        if (skipPackages && isPackageName(n))
            return true;
        if (paths.isEmpty() && pathRx.isEmpty())
            return false;
        const QString p = foldPath(path);
        if (paths.contains(p))
            return true;
        for (const QRegularExpression &rx : pathRx)
            if (rx.match(p).hasMatch())
                return true;
        return false;
    }

    // Bundles Finder shows as files (the folder is a package by its extension; asking Launch
    // Services for each of 300 000 folders would be far too slow).
    static bool isPackageName(const QString &foldedName)
    {
        const qsizetype dot = foldedName.lastIndexOf(QLatin1Char('.'));
        if (dot <= 0)
            return false;
        static const QSet<QString> ext = {
            QStringLiteral("app"), QStringLiteral("bundle"), QStringLiteral("framework"), QStringLiteral("plugin"),
            QStringLiteral("kext"), QStringLiteral("appex"), QStringLiteral("xpc"), QStringLiteral("photoslibrary"),
            QStringLiteral("xcarchive"), QStringLiteral("prefpane"), QStringLiteral("qlgenerator"),
            QStringLiteral("mdimporter"), QStringLiteral("saver"), QStringLiteral("wdgt"), QStringLiteral("playground"),
            QStringLiteral("xcodeproj"), QStringLiteral("xcworkspace"), QStringLiteral("rtfd"), QStringLiteral("pages"),
            QStringLiteral("numbers"), QStringLiteral("key"), QStringLiteral("band"), QStringLiteral("logicx"),
            QStringLiteral("imovielibrary"), QStringLiteral("fcpbundle"), QStringLiteral("musiclibrary"),
            QStringLiteral("tvlibrary"), QStringLiteral("photolibrary"), QStringLiteral("dsym")};
        return ext.contains(foldedName.mid(dot + 1));
    }
};

// The folders directly inside `dir`; symbolic links (and Windows junctions) are not followed, so
// nothing is listed twice and there are no loops.
void listFolders(const QString &dir, QStringList &out)
{
#ifdef Q_OS_WIN
    QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System | QDir::NoSymLinks);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (!fi.isJunction())
            out << fi.fileName();
    }
#else
    const QByteArray d = QFile::encodeName(dir);
    DIR *h = opendir(d.constData());
    if (!h)
        return;
    while (const dirent *e = readdir(h)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0)))
            continue;
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            const QByteArray full = d.endsWith('/') ? d + n : d + '/' + n;
            struct stat st;
            isDir = lstat(full.constData(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (isDir)
            out << QFile::decodeName(n);
    }
    closedir(h);
#endif
}

} // namespace

QString FolderIndex::Options::key() const
{
    return QStringLiteral("%1\n%2\n%3\n%4").arg(kFormat).arg(roots.join(QLatin1Char('|')), exclude.join(QLatin1Char('|'))).arg(skipPackages);
}

QStringList FolderIndex::foreignMounts()
{
    QStringList out;
#ifndef Q_OS_WIN
    static const QSet<QString> foreign = {
        QStringLiteral("nfs"), QStringLiteral("nfs4"), QStringLiteral("smbfs"), QStringLiteral("cifs"),
        QStringLiteral("smb3"), QStringLiteral("afpfs"), QStringLiteral("webdav"), QStringLiteral("ftp"),
        QStringLiteral("sshfs"), QStringLiteral("9p"), QStringLiteral("autofs"), QStringLiteral("squashfs"),
        QStringLiteral("osxfuse"), QStringLiteral("macfuse"), QStringLiteral("davfs"), QStringLiteral("gvfs"),
        QStringLiteral("proc"), QStringLiteral("sysfs"), QStringLiteral("devfs"), QStringLiteral("devtmpfs")};
    for (const QStorageInfo &v : QStorageInfo::mountedVolumes()) {
        const QString type = QString::fromLatin1(v.fileSystemType()).toLower();
        if (v.rootPath() != QLatin1String("/") && (foreign.contains(type) || type.startsWith(QLatin1String("fuse"))))
            out << v.rootPath();
    }
#endif
    return out;
}

FolderIndex FolderIndex::scan(const Options &options, std::atomic<int> *progress, const std::atomic<bool> *cancel)
{
    FolderIndex idx;
    QStringList roots;
    for (const QString &r : options.roots)
        if (!r.trimmed().isEmpty())
            roots << cleanRoot(r.trimmed());
    roots.removeDuplicates();
    QStringList mounts;
    for (const QString &m : foreignMounts()) // a root that is itself such a mount stays
        if (!roots.contains(cleanRoot(m), kPathCase))
            mounts << m;
    const Rules rules(options, mounts);

    auto add = [&idx](int parent, const QString &name, int depth) {
        const QByteArray u = name.toUtf8(), f = fold(name).toUtf8();
        idx.m_nodes.push_back(Node{parent, 0, 0, quint32(idx.m_names.size()), quint32(idx.m_folded.size()),
                                   quint16(qMin<qsizetype>(u.size(), 0xFFFF)), quint16(qMin<qsizetype>(f.size(), 0xFFFF)),
                                   quint16(qMin(depth, 0xFFFF))});
        idx.m_names += u.left(0xFFFF);
        idx.m_folded += f.left(0xFFFF);
    };
    for (const QString &r : roots)
        add(-1, r, 0);
    idx.m_roots = int(roots.size());

    // Breadth first: each folder's children are appended together, so they stay consecutive.
    QStringList kids;
    for (size_t i = 0; i < idx.m_nodes.size(); ++i) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            return {};
        const QString dir = idx.path(int(i));
        kids.clear();
        listFolders(dir, kids);
        kids.erase(std::remove_if(kids.begin(), kids.end(), [&](const QString &k) { return rules.excluded(k, join(dir, k)); }),
                   kids.end());
        std::sort(kids.begin(), kids.end(), [](const QString &a, const QString &b) { return util::naturalCompare(a, b) < 0; });
        const int depth = idx.m_nodes[i].depth + 1;
        idx.m_nodes[i].first = qint32(idx.m_nodes.size());
        idx.m_nodes[i].count = qint32(kids.size());
        for (const QString &k : kids)
            add(int(i), k, depth);
        if (progress)
            progress->store(int(idx.m_nodes.size()), std::memory_order_relaxed);
    }
    idx.m_nodes.shrink_to_fit();
    idx.m_names.squeeze();
    idx.m_folded.squeeze();
    return idx;
}

QString FolderIndex::name(int n) const
{
    return QString::fromUtf8(m_names.constData() + m_nodes[n].nameOff, m_nodes[n].nameLen);
}

QString FolderIndex::path(int n) const
{
    QList<int> chain;
    for (int i = n; i >= 0; i = m_nodes[i].parent)
        chain.prepend(i);
    QString p;
    for (int i : chain)
        p = p.isEmpty() ? name(i) : join(p, name(i));
    return p;
}

int FolderIndex::childNamed(int parent, const QByteArray &foldedName) const
{
    const Node &p = m_nodes[parent];
    for (int c = p.first; c < p.first + p.count; ++c)
        if (folded(c) == foldedName)
            return c;
    return -1;
}

int FolderIndex::findNearest(const QString &path) const
{
    const QString p = cleanRoot(path);
    int root = -1;
    qsizetype rootLen = -1;
    for (int r = 0; r < m_roots; ++r) {
        const QString rp = name(r);
        const bool inside = p.compare(rp, kPathCase) == 0
                         || (p.startsWith(rp.endsWith(QLatin1Char('/')) ? rp : rp + QLatin1Char('/'), kPathCase));
        if (inside && rp.size() > rootLen) {
            root = r;
            rootLen = rp.size();
        }
    }
    if (root < 0)
        return -1;
    int n = root;
    for (const QString &seg : p.mid(rootLen).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const int c = childNamed(n, fold(seg).toUtf8());
        if (c < 0)
            break;
        n = c;
    }
    return n;
}

int FolderIndex::find(const QString &path) const
{
    const int n = findNearest(path);
    return n >= 0 && foldPath(this->path(n)) == foldPath(cleanRoot(path)) ? n : -1;
}

FolderIndex::Matches FolderIndex::match(const QString &query, const QHash<int, int> &boost, int limit) const
{
    Matches out;
    QStringList parts = fold(QDir::fromNativeSeparators(query.trimmed())).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return out;
    const QByteArray last = parts.takeLast().toUtf8();
    QList<QByteArray> before;
    for (const QString &s : parts)
        before << s.toUtf8();
    const std::string_view needle(last.constData(), size_t(last.size()));
    auto sv = [](QByteArrayView v) { return std::string_view(v.data(), size_t(v.size())); };

    struct Hit {
        int node, score, boost, depth;
    };
    std::vector<Hit> hits;
    for (int n = m_roots; n < size(); ++n) {
        const std::string_view name = sv(folded(n));
        const size_t pos = name.find(needle);
        if (pos == std::string_view::npos)
            continue;
        if (!before.isEmpty()) { // each earlier part in an ancestor, in order upward
            int a = m_nodes[n].parent;
            bool found = true;
            for (qsizetype k = before.size() - 1; k >= 0 && found; --k) {
                const std::string_view part(before[k].constData(), size_t(before[k].size()));
                while (a >= 0 && sv(folded(a)).find(part) == std::string_view::npos)
                    a = m_nodes[a].parent;
                found = a >= 0;
                if (found)
                    a = m_nodes[a].parent;
            }
            if (!found)
                continue;
        }
        int score = 0;
        if (pos == 0)
            score = name.size() == needle.size() ? 3 : 2;
        else if (std::string_view(" -_.()[]{}+,@#").find(name[pos - 1]) != std::string_view::npos)
            score = 1;
        hits.push_back({n, score, boost.value(n), m_nodes[n].depth});
    }
    out.total = int(hits.size());
    const auto better = [](const Hit &a, const Hit &b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.boost != b.boost)
            return a.boost > b.boost;
        if (a.depth != b.depth)
            return a.depth < b.depth;
        return a.node < b.node;
    };
    const size_t keep = std::min(hits.size(), size_t(qMax(limit, 1)));
    std::partial_sort(hits.begin(), hits.begin() + keep, hits.end(), better);
    out.best.reserve(keep);
    for (size_t i = 0; i < keep; ++i)
        out.best.push_back(hits[i].node);
    return out;
}

bool FolderIndex::save(const QString &file, const QString &key, qint64 scannedAt) const
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QDataStream s(&f);
    s.setVersion(QDataStream::Qt_6_5);
    s << kMagic << kFormat << key << scannedAt << qint32(m_roots) << quint64(m_nodes.size()) << m_names << m_folded;
    s.writeRawData(reinterpret_cast<const char *>(m_nodes.data()), qsizetype(m_nodes.size() * sizeof(Node)));
    return s.status() == QDataStream::Ok && f.commit();
}

bool FolderIndex::load(const QString &file, const QString &key, FolderIndex &out, qint64 *scannedAt)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QDataStream s(&f);
    s.setVersion(QDataStream::Qt_6_5);
    quint32 magic = 0, format = 0;
    QString storedKey;
    qint64 at = 0;
    qint32 roots = 0;
    quint64 count = 0;
    s >> magic >> format;
    if (magic != kMagic || format != kFormat)
        return false;
    s >> storedKey >> at >> roots >> count;
    if (s.status() != QDataStream::Ok || storedKey != key || count > 50'000'000 || roots < 0 || quint64(roots) > count)
        return false;
    FolderIndex idx;
    s >> idx.m_names >> idx.m_folded;
    idx.m_nodes.resize(count);
    const qsizetype bytes = qsizetype(count * sizeof(Node));
    if (s.status() != QDataStream::Ok || s.readRawData(reinterpret_cast<char *>(idx.m_nodes.data()), bytes) != bytes)
        return false;
    for (quint64 i = 0; i < count; ++i) { // a damaged file must not make us read out of bounds
        const Node &n = idx.m_nodes[i];
        if (n.parent >= qint32(i) || n.parent < -1 || (n.parent < 0) != (i < quint64(roots)) || n.count < 0
            || quint64(n.first) + quint64(n.count) > count || quint64(n.nameOff) + n.nameLen > quint64(idx.m_names.size())
            || quint64(n.foldOff) + n.foldLen > quint64(idx.m_folded.size()))
            return false;
    }
    idx.m_roots = roots;
    out = std::move(idx);
    if (scannedAt)
        *scannedAt = at;
    return true;
}
