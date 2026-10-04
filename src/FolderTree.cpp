#include "FolderTree.h"
#include "App.h"
#include "FolderEvents.h"
#include "Permissions.h"
#include "Settings.h"
#include "Theme.h"
#include "Util.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QListWidget>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSettings>
#include <QStorageInfo>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QThread>
#include <QListView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr qint64 kMaxAgeOpen = 30 * 60 * 1000;          // a panel opening rescans an older index
constexpr qint64 kMaxAgeIdle = 6 * 60 * 60 * 1000;      // the background keeps it at most this old
constexpr int kReleaseAfter = 2 * 60 * 1000;            // memory freed after the panel closed
constexpr int kMaxVisits = 1000;
// FSEvents: replaying the journal takes ~2.6 s per million events on the developer's Mac (about 2.5
// days of use there); beyond this many a full scan (8.3 s) is quicker.
constexpr quint64 kMaxReplay = 3'000'000;
constexpr int kMaxPending = 50'000; // folders changed at once: scan instead
const QString kVisitsKey = QStringLiteral("folderTree/visits");
const QString kGrantedKey = QStringLiteral("folderTree/granted");

qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

#ifdef Q_OS_MACOS
// Folders macOS asks about before an app may read them (with Full Disk Access it never asks).
QStringList protectedFolders()
{
    return {QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
            QDir::homePath() + QStringLiteral("/Library/Containers"),
            QDir::homePath() + QStringLiteral("/Library/Group Containers")};
}
#endif

QStringList wholeDrive()
{
#ifdef Q_OS_WIN
    QStringList roots;
    for (const QFileInfo &d : QDir::drives()) {
        const QString p = d.absoluteFilePath();
        if (GetDriveTypeW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(p).utf16())) == DRIVE_FIXED)
            roots << p;
    }
    return roots;
#else
    return {QStringLiteral("/")};
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// FolderTree: the index, its cache and the background scans

FolderTree *FolderTree::instance()
{
    static FolderTree *t = new FolderTree;
    return t;
}

FolderTree::FolderTree()
{
    m_release.setSingleShot(true);
    m_release.setInterval(kReleaseAfter);
    connect(&m_release, &QTimer::timeout, this, [this] {
        if (m_users == 0 && !m_scanning && !m_updating) {
            unwatch();
            m_index.reset();
            m_indexKey.clear();
        }
    });
    m_progressTimer.setInterval(150);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] { emit progress(m_progress->load()); });
    m_saveVisits.setSingleShot(true);
    m_saveVisits.setInterval(3000);
    connect(&m_saveVisits, &QTimer::timeout, this, &FolderTree::saveVisits);
    const QVariantMap stored = QSettings().value(kVisitsKey).toMap();
    for (auto it = stored.cbegin(); it != stored.cend(); ++it)
        m_visits.insert(it.key(), it.value().toInt());
    // A folder made, renamed, moved or trashed in the app: the next panel rescans (FSEvents brings it anyway).
    connect(App::instance(), &App::undoChanged, this, [this] {
        if (!m_stream)
            markStale();
    });
    m_applyTimer.setSingleShot(true);
    m_applyTimer.setInterval(300);
    connect(&m_applyTimer, &QTimer::timeout, this, &FolderTree::applyEvents);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        m_cancel->store(true);
        unwatch();
        if (m_saveVisits.isActive())
            saveVisits();
    });
}

void FolderTree::saveVisits()
{
    m_saveVisits.stop();
    QVariantMap map;
    for (auto it = m_visits.cbegin(); it != m_visits.cend(); ++it)
        map.insert(it.key(), it.value());
    QSettings().setValue(kVisitsKey, map);
}

QStringList FolderTree::defaultExclude()
{
    QStringList l = {QStringLiteral("node_modules"), QStringLiteral(".git"), QStringLiteral(".svn"), QStringLiteral(".hg"),
                     QStringLiteral("__pycache__")};
#ifdef Q_OS_MACOS
    l << QStringLiteral("/System") << QStringLiteral("/Volumes") << QStringLiteral("/dev") << QStringLiteral("/private/var")
      << QStringLiteral("/cores") << QStringLiteral("/Library/Developer/CoreSimulator") << QStringLiteral("~/Library")
      << QStringLiteral("~/.Trash");
#elif defined(Q_OS_WIN)
    l << QStringLiteral("$RECYCLE.BIN") << QStringLiteral("System Volume Information") << QStringLiteral("*:/Windows")
      << QStringLiteral("~/AppData");
#else
    l << QStringLiteral("/proc") << QStringLiteral("/sys") << QStringLiteral("/dev") << QStringLiteral("/run")
      << QStringLiteral("/snap") << QStringLiteral("/tmp") << QStringLiteral("/var/cache") << QStringLiteral("/var/lib")
      << QStringLiteral("/lost+found") << QStringLiteral("~/.cache") << QStringLiteral("~/.local/share/Trash");
#endif
    return l;
}

FolderIndex::Options FolderTree::options()
{
    FolderIndex::Options o;
    const QString drive = instance()->m_drive;
    o.roots = drive.isEmpty() ? Settings::instance()->value(Settings::FolderTreeRoots).toStringList() : QStringList{drive};
    o.roots.removeAll(QString());
    if (o.roots.isEmpty())
        o.roots = wholeDrive();
    o.exclude = Settings::instance()->value(Settings::FolderTreeExclude).toStringList();
#ifdef Q_OS_MACOS
    o.skipPackages = true;
    if (!Permissions::hasFullDiskAccess()) {
        const QStringList granted = QSettings().value(kGrantedKey).toStringList();
        for (const QString &p : protectedFolders())
            if (!granted.contains(p))
                o.exclude << p;
    }
#endif
    return o;
}

QString FolderTree::cacheFile(const QString &key)
{
    const QByteArray h = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/folder-tree-")
         + QString::fromLatin1(h) + QStringLiteral(".bin");
}

void FolderTree::startScan(bool lowPriority)
{
    if (m_scanning)
        return;
    m_scanning = true;
    m_staleDuringScan = false;
    m_progress->store(0);
    unwatch(); // the scan replaces what it would have brought; changes during the scan replay after it
    const FolderIndex::Options opts = options();
    const QString key = opts.key(), file = cacheFile(key);
    FolderIndex::Journal journal;
    if (FolderEvents::available()) {
        journal.id = FolderEvents::journalId(opts.roots);
        journal.eventId = journal.id.isEmpty() ? 0 : FolderEvents::currentId();
    }
    auto counter = m_progress;
    auto cancel = m_cancel;
    auto *watcher = new QFutureWatcher<std::shared_ptr<FolderIndex>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, key, journal] {
        const std::shared_ptr<FolderIndex> result = watcher->result();
        watcher->deleteLater();
        m_scanning = false;
        m_progressTimer.stop();
        const bool current = key == options().key();
        if (result && current) {
            m_index = result;
            m_indexKey = key;
            m_scannedAt = now();
            m_journal = journal;
            m_savedEventId = journal.eventId;
            m_stale = m_staleDuringScan;
            emit indexChanged();
        }
        if (m_users == 0) {
            m_index.reset();
            m_indexKey.clear();
        } else if (m_stale || !current) {
            startScan(false);
        } else {
            watch();
            emit progress(m_progress->load()); // the "인덱싱 중…" note goes away
        }
    });
    watcher->setFuture(QtConcurrent::run([opts, key, file, counter, cancel, lowPriority, journal]() -> std::shared_ptr<FolderIndex> {
        if (lowPriority)
            QThread::currentThread()->setPriority(QThread::LowestPriority);
        auto idx = std::make_shared<FolderIndex>(FolderIndex::scan(opts, counter.get(), cancel.get()));
        if (lowPriority)
            QThread::currentThread()->setPriority(QThread::NormalPriority);
        if (cancel->load() || idx->rootCount() == 0)
            return {};
        idx->save(file, key, now(), journal);
        // Caches made for other settings or drives (⌘D): the 8 newest stay, for a month.
        const QDir dir = QFileInfo(file).absoluteDir();
        int kept = 0;
        for (const QFileInfo &old : dir.entryInfoList({QStringLiteral("folder-tree-*.bin")}, QDir::Files, QDir::Time))
            if (old.absoluteFilePath() != QFileInfo(file).absoluteFilePath()
                && (++kept > 8 || old.lastModified().daysTo(QDateTime::currentDateTime()) > 30))
                QFile::remove(old.absoluteFilePath());
        return idx;
    }));
    m_progressTimer.start();
    emit progress(0);
}

void FolderTree::acquire(const QString &folder)
{
    ++m_users;
    m_release.stop();
    load(folder);
}

void FolderTree::setDrive(const QString &root)
{
    if (root == m_drive)
        return;
    m_drive = root;
    load(QString());
    emit indexChanged();
}

QList<FolderTree::Drive> FolderTree::drives()
{
    QStringList defaults = Settings::instance()->value(Settings::FolderTreeRoots).toStringList();
    defaults.removeAll(QString());
    const bool configured = !defaults.isEmpty();
    if (!configured)
        defaults = wholeDrive();
    auto same = [](const QString &a, const QString &b) { return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0; };
    QList<Drive> out;
    bool merged = false;
    for (const QStorageInfo &si : QStorageInfo::mountedVolumes()) {
        if (!util::isUserVolume(si))
            continue;
        Drive d;
        d.root = si.rootPath();
        d.path = QDir::toNativeSeparators(d.root);
        d.label = si.displayName();
        if (d.label.isEmpty() || d.label == d.root)
            d.label = d.root == QLatin1String("/") ? Gifiles::tr("컴퓨터") : util::displayName(d.root);
        if (defaults.size() == 1 && same(defaults.first(), d.root)) {
            d.root.clear(); // the default tree is this drive
            merged = true;
        }
        out << d;
    }
    if (!merged) {
        QStringList shown;
        for (const QString &r : defaults)
            shown << QDir::toNativeSeparators(r);
        out.prepend({QString(), configured ? Gifiles::tr("설정한 폴더") : Gifiles::tr("모든 드라이브"), shown.join(QStringLiteral(", "))});
    }
    return out;
}

void FolderTree::load(const QString &folder)
{
    const QString key = options().key();
    if (!m_index || m_indexKey != key) {
        unwatch();
        FolderIndex idx;
        qint64 at = 0;
        FolderIndex::Journal journal;
        if (FolderIndex::load(cacheFile(key), key, idx, &at, &journal)) {
            m_index = std::make_shared<FolderIndex>(std::move(idx));
            m_indexKey = key;
            m_scannedAt = at;
            m_journal = journal;
        } else {
            m_index.reset();
            m_indexKey.clear();
            m_scannedAt = 0;
            m_journal = {};
        }
        m_savedEventId = m_journal.eventId;
    }
    if (m_index && !m_stale && watch())
        return; // FSEvents keeps it up to date, also for the time the app wasn't running
    // A folder's time changes when something inside it is added, renamed or removed.
    const QFileInfo fi(folder);
    if (m_index && fi.exists() && fi.lastModified().toMSecsSinceEpoch() > m_scannedAt)
        m_stale = true;
    if (!m_index || m_stale || now() - m_scannedAt > kMaxAgeOpen)
        startScan(false);
}

bool FolderTree::watch()
{
    if (m_stream)
        return true;
    if (!FolderEvents::available() || !m_index || m_journal.eventId == 0 || m_scanning)
        return false;
    const FolderIndex::Options opts = options();
    // Another journal (the volume's was reset) or too long ago to replay: scan.
    if (m_journal.id != FolderEvents::journalId(opts.roots) || FolderEvents::currentId() - m_journal.eventId > kMaxReplay)
        return false;
    m_replayed = false;
    m_pendingId = m_journal.eventId;
    m_stream = std::make_unique<FolderEvents::Stream>(opts.roots, m_journal.eventId, [this](const FolderEvents::Batch &b) {
        if (b.reset || m_pendingChanged.size() + m_pendingDeep.size() > kMaxPending) {
            unwatch();
            m_journal = {};
            if (m_users > 0)
                startScan(false);
            return;
        }
        for (const QString &p : b.changed)
            m_pendingChanged.insert(p);
        for (const QString &p : b.deep)
            m_pendingDeep.insert(p);
        m_pendingId = qMax(m_pendingId, b.lastId);
        m_replayed = m_replayed || b.historyDone;
        if (m_replayed)
            m_applyTimer.start();
    });
    return true;
}

void FolderTree::unwatch()
{
    if (!m_stream)
        return;
    m_stream.reset();
    m_applyTimer.stop();
    m_pendingChanged.clear();
    m_pendingDeep.clear();
    // Where the journal was replayed to, so the next start replays only what came after.
    if (m_index && m_journal.eventId > m_savedEventId) {
        m_savedEventId = m_journal.eventId;
        m_index->save(cacheFile(m_indexKey), m_indexKey, m_scannedAt, m_journal);
    }
}

void FolderTree::applyEvents()
{
    if (!m_stream || !m_index || m_scanning)
        return;
    if (m_updating)
        return m_applyTimer.start(); // after the update under way
    QStringList changed = m_pendingChanged.values(), deep = m_pendingDeep.values();
    const quint64 id = m_pendingId;
    m_pendingChanged.clear();
    m_pendingDeep.clear();
    // Only folders of the tree matter (most events are inside left-out folders such as ~/Library).
    const std::shared_ptr<const FolderIndex> old = m_index;
    changed.erase(std::remove_if(changed.begin(), changed.end(), [&](const QString &p) { return old->find(p) < 0; }), changed.end());
    deep.erase(std::remove_if(deep.begin(), deep.end(), [&](const QString &p) { return old->findNearest(p) < 0; }), deep.end());
    if (changed.isEmpty() && deep.isEmpty()) {
        m_journal.eventId = qMax(m_journal.eventId, id);
        return;
    }
    m_updating = true;
    const FolderIndex::Options opts = options();
    const QString key = m_indexKey, file = cacheFile(key);
    const qint64 scannedAt = m_scannedAt;
    const FolderIndex::Journal journal{id, m_journal.id};
    auto cancel = m_cancel;
    auto *watcher = new QFutureWatcher<std::shared_ptr<FolderIndex>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, old, key, id] {
        const std::shared_ptr<FolderIndex> result = watcher->result();
        watcher->deleteLater();
        m_updating = false;
        if (m_index != old || m_indexKey != key) // scanned or switched meanwhile
            return;
        m_journal.eventId = qMax(m_journal.eventId, id);
        if (result) {
            m_index = result;
            m_savedEventId = m_journal.eventId;
            emit indexChanged();
        }
        if (!m_pendingChanged.isEmpty() || !m_pendingDeep.isEmpty())
            m_applyTimer.start();
    });
    watcher->setFuture(QtConcurrent::run([old, opts, changed, deep, key, file, scannedAt, journal, cancel]() -> std::shared_ptr<FolderIndex> {
        bool differs = false;
        auto idx = std::make_shared<FolderIndex>(FolderIndex::update(*old, opts, changed, deep, nullptr, cancel.get(), &differs));
        if (cancel->load() || !differs)
            return {};
        idx->save(file, key, scannedAt, journal);
        return idx;
    }));
}

void FolderTree::release()
{
    m_users = qMax(0, m_users - 1);
    if (m_users == 0)
        m_release.start();
}

void FolderTree::prefetch()
{
    auto check = [this] {
        if (m_scanning || !m_drive.isEmpty()) // a drive chosen with ⌘D isn't kept fresh in the background
            return;
        const QFileInfo cache(cacheFile(options().key()));
        if (FolderEvents::available() && cache.exists() && !m_stale) {
            // Catch up with the journal now and then, so opening the panel has little to replay;
            // load() scans instead when the cache can't follow the journal.
            acquire(QString());
            release();
            return;
        }
        if (m_stale || !cache.exists() || cache.lastModified().msecsTo(QDateTime::currentDateTime()) > kMaxAgeIdle)
            startScan(true);
    };
    QTimer::singleShot(20 * 1000, this, check); // after the windows have settled
    m_prefetch.setInterval(60 * 60 * 1000);
    connect(&m_prefetch, &QTimer::timeout, this, check);
    m_prefetch.start();
}

void FolderTree::reindex()
{
    m_stale = true;
    if (m_scanning)
        m_staleDuringScan = true;
    else
        startScan(false);
}

void FolderTree::markStale()
{
    m_stale = true;
    if (m_scanning)
        m_staleDuringScan = true;
    else if (m_users > 0)
        startScan(false);
}

void FolderTree::noteVisit(const QString &path)
{
    if (path.isEmpty())
        return;
    ++m_visits[path];
    if (m_visits.size() > kMaxVisits) { // keep the most visited
        QList<int> counts = m_visits.values();
        std::sort(counts.begin(), counts.end(), std::greater<int>());
        const int cut = counts[kMaxVisits * 4 / 5];
        for (auto it = m_visits.begin(); it != m_visits.end();)
            it = it.value() <= cut && it.key() != path ? m_visits.erase(it) : std::next(it);
    }
    m_saveVisits.start();
#ifdef Q_OS_MACOS
    // Opened in the browser: macOS has asked already, so the scan may go in from now on.
    for (const QString &p : protectedFolders()) {
        if (!util::isInside(path, p))
            continue;
        QStringList granted = QSettings().value(kGrantedKey).toStringList();
        if (!granted.contains(p) && !Permissions::hasFullDiskAccess()) {
            granted << p;
            QSettings().setValue(kGrantedKey, granted);
            markStale();
        }
    }
#endif
}

QHash<int, int> FolderTree::boost() const
{
    QHash<int, int> out;
    if (!m_index)
        return out;
    for (auto it = m_visits.cbegin(); it != m_visits.cend(); ++it)
        if (const int n = m_index->find(it.key()); n >= 0)
            out.insert(n, it.value());
    return out;
}

// ---------------------------------------------------------------------------
// FolderTreeModel

void FolderTreeModel::setIndex(std::shared_ptr<const FolderIndex> index)
{
    beginResetModel();
    m_index = std::move(index);
    m_order.clear();
    m_rowOf.clear();
    if (m_index) {
        // Depth first: each folder followed by everything inside it.
        m_order.reserve(size_t(m_index->size()));
        m_rowOf.assign(size_t(m_index->size()), -1);
        std::vector<int> stack;
        for (int r = m_index->rootCount() - 1; r >= 0; --r)
            stack.push_back(r);
        while (!stack.empty()) {
            const int n = stack.back();
            stack.pop_back();
            m_rowOf[size_t(n)] = int(m_order.size());
            m_order.push_back(n);
            for (int c = m_index->firstChild(n) + m_index->childCount(n) - 1; c >= m_index->firstChild(n); --c)
                stack.push_back(c);
        }
    }
    endResetModel();
}

QModelIndex FolderTreeModel::indexOf(int node) const
{
    return node >= 0 && size_t(node) < m_rowOf.size() ? index(m_rowOf[size_t(node)]) : QModelIndex();
}

int FolderTreeModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_order.size()); }

QVariant FolderTreeModel::data(const QModelIndex &index, int role) const
{
    const int n = nodeOf(index);
    if (n < 0)
        return {};
    if (role == Qt::DisplayRole)
        return n < m_index->rootCount() ? QDir::toNativeSeparators(m_index->name(n)) : m_index->name(n);
    if (role == Qt::ToolTipRole)
        return QDir::toNativeSeparators(m_index->path(n));
    return {};
}

namespace {

// Indentation, NCD-like tree lines, a folder glyph and the name.
class FolderTreeDelegate : public QStyledItemDelegate {
public:
    FolderTreeDelegate(FolderTreeModel *model, QObject *parent) : QStyledItemDelegate(parent), m_model(model) {}

    static constexpr int kIndent = 18, kRow = 22, kPad = 8;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(200, qMax(kRow, option.fontMetrics.height() + 6));
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const FolderIndex *f = m_model->folders();
        const int n = m_model->nodeOf(index);
        if (!f || n < 0)
            return;
        const Theme::Colors &c = Theme::colors();
        const QRect r = option.rect;
        const bool selected = option.state & QStyle::State_Selected;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (selected || (option.state & QStyle::State_MouseOver)) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? c.treeSelection : c.treeHover);
            p->drawRoundedRect(r.adjusted(2, 0, -2, -1), 5, 5);
        }
        auto isLast = [f](int node) {
            const int parent = f->parent(node);
            return parent < 0 ? node == f->rootCount() - 1 : node == f->firstChild(parent) + f->childCount(parent) - 1;
        };
        // Lines: one per open ancestor level, then this folder's own branch.
        const int depth = f->depth(n);
        p->setRenderHint(QPainter::Antialiasing, false);
        p->setPen(QPen(selected ? c.treeSelText : c.treeLine, 1));
        const int midY = r.center().y();
        auto levelX = [&](int level) { return r.left() + kPad + level * kIndent + kIndent / 2; };
        int a = n;
        for (int level = depth - 1; level >= 0; --level) {
            const int child = a; // the node at depth level + 1 on the way up
            a = f->parent(a);
            const int x = levelX(level);
            if (level == depth - 1) {
                p->drawLine(x, r.top(), x, isLast(child) ? midY : r.bottom());
                p->drawLine(x, midY, x + kIndent / 2 + 1, midY);
            } else if (!isLast(child)) {
                p->drawLine(x, r.top(), x, r.bottom());
            }
        }
        p->setRenderHint(QPainter::Antialiasing);
        const int iconX = r.left() + kPad + depth * kIndent + 2;
        const QColor iconColor = selected ? c.treeSelText : c.treeSecondary;
        Theme::icon(QStringLiteral("folder"), iconColor, 16).paint(p, QRect(iconX, midY - 8, 16, 16));
        const QRect textRect(iconX + 22, r.top(), r.right() - iconX - 26, r.height());
        p->setPen(selected ? c.treeSelText : c.treeText);
        p->setFont(option.font);
        const QString name = option.fontMetrics.elidedText(index.data().toString(), Qt::ElideMiddle, textRect.width());
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, name);
        // The selected folder's whole path after its name, dimmed.
        const int gap = option.fontMetrics.horizontalAdvance(name) + 14;
        if (selected && n >= f->rootCount() && gap < textRect.width() - 40) {
            const QRect pathRect = textRect.adjusted(gap, 0, 0, 0);
            p->setOpacity(0.5);
            p->drawText(pathRect, Qt::AlignVCenter | Qt::AlignLeft,
                        option.fontMetrics.elidedText(QDir::toNativeSeparators(f->path(n)), Qt::ElideMiddle, pathRect.width()));
            p->setOpacity(1);
        }
        p->restore();
    }

private:
    FolderTreeModel *m_model;
};

// A drive: its glyph and name, the path dimmed on the right.
class DriveDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(200, qMax(28, option.fontMetrics.height() + 10));
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const Theme::Colors &c = Theme::colors();
        const QRect r = option.rect;
        const bool selected = option.state & QStyle::State_Selected;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (selected || (option.state & QStyle::State_MouseOver)) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? c.treeSelection : c.treeHover);
            p->drawRoundedRect(r.adjusted(0, 1, 0, -1), 6, 6);
        }
        const QColor fg = selected ? c.treeSelText : c.treeText;
        Theme::icon(QStringLiteral("drive"), selected ? c.treeSelText : c.treeSecondary, 16).paint(p, QRect(r.left() + 10, r.center().y() - 8, 16, 16));
        const QRect text = r.adjusted(34, 0, -10, 0);
        const QString path = index.data(Qt::UserRole + 1).toString();
        const int pathW = qMin(option.fontMetrics.horizontalAdvance(path), text.width() / 2);
        p->setFont(option.font);
        p->setPen(fg);
        p->drawText(text.adjusted(0, 0, -pathW - 12, 0), Qt::AlignVCenter | Qt::AlignLeft,
                    option.fontMetrics.elidedText(index.data().toString(), Qt::ElideMiddle, text.width() - pathW - 12));
        p->setOpacity(0.5);
        p->drawText(text, Qt::AlignVCenter | Qt::AlignRight, option.fontMetrics.elidedText(path, Qt::ElideMiddle, pathW));
        p->restore();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// FolderTreePanel

void FolderTreeQuery::inputMethodEvent(QInputMethodEvent *e)
{
    QLineEdit::inputMethodEvent(e);
    if (e->preeditString() != m_preedit) {
        m_preedit = e->preeditString();
        emit queryChanged();
    }
}

FolderTreePanel::FolderTreePanel(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("folderTreePanel"));
    setAttribute(Qt::WA_StyledBackground);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(12, 10, 12, 6);
    l->setSpacing(8);
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
#ifdef Q_OS_MACOS
    row->setContentsMargins(66, 0, 0, 0); // it covers the window: the query line sits beside the traffic lights
#endif
    m_edit = new FolderTreeQuery(this);
    m_edit->setObjectName(QStringLiteral("folderTreeQuery"));
    m_edit->setAttribute(Qt::WA_InputMethodEnabled, true);
    m_edit->installEventFilter(this);
    m_case = new QToolButton(this);
    m_case->setObjectName(QStringLiteral("folderTreeCase"));
    m_case->setText(QStringLiteral("Aa"));
    m_case->setCheckable(true);
    m_case->setFocusPolicy(Qt::NoFocus);
    m_case->setChecked(Settings::instance()->flag(Settings::FolderTreeCase));
    connect(m_case, &QToolButton::toggled, this, [this](bool on) {
        if (Settings::instance()->flag(Settings::FolderTreeCase) != on)
            Settings::instance()->setValue(Settings::FolderTreeCase, on);
        runQuery(true);
    });
    connect(Settings::instance(), &Settings::changed, m_case, [this](const QString &k) {
        if (k.isEmpty() || k == QLatin1String(Settings::FolderTreeCase))
            m_case->setChecked(Settings::instance()->flag(Settings::FolderTreeCase));
    });
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("secondary"));
    row->addWidget(m_edit, 1);
    row->addWidget(m_case);
    row->addWidget(m_status);
    l->addLayout(row);

    m_view = new QListView(this);
    m_view->setObjectName(QStringLiteral("folderTree"));
    m_view->setUniformItemSizes(true); // 100 000+ rows: only the visible ones are measured
    m_view->setFocusPolicy(Qt::NoFocus); // the keyboard stays in the query line
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setMouseTracking(true);
    m_model = new FolderTreeModel(this);
    m_view->setModel(m_model);
    m_view->setItemDelegate(new FolderTreeDelegate(m_model, m_view));
    l->addWidget(m_view, 1);
    m_keys = new QLabel(this);
    m_keys->setObjectName(QStringLiteral("folderTreeKeys"));
    m_keys->setWordWrap(true);
    l->addWidget(m_keys);
    m_busy = new QLabel(this);
    m_busy->setObjectName(QStringLiteral("folderTreeBusy"));
    m_busy->hide();
    m_drives = new QFrame(this);
    m_drives->setObjectName(QStringLiteral("folderTreeDrives"));
    m_drives->setAttribute(Qt::WA_StyledBackground);
    auto *dl = new QVBoxLayout(m_drives);
    dl->setContentsMargins(6, 8, 6, 6);
    dl->setSpacing(4);
    auto *title = new QLabel(Gifiles::tr("드라이브"), m_drives);
    title->setObjectName(QStringLiteral("secondary"));
    title->setContentsMargins(8, 0, 8, 0);
    dl->addWidget(title);
    m_driveList = new QListWidget(m_drives);
    m_driveList->setObjectName(QStringLiteral("folderTreeDriveList"));
    m_driveList->setFocusPolicy(Qt::NoFocus); // the keyboard stays in the query line (driveKey)
    m_driveList->setFrameShape(QFrame::NoFrame);
    m_driveList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_driveList->setMouseTracking(true);
    m_driveList->setItemDelegate(new DriveDelegate(m_driveList));
    dl->addWidget(m_driveList);
    m_drives->hide();
    connect(m_driveList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        m_driveList->setCurrentItem(item);
        pickDrive();
    });
    connect(m_view, &QListView::pressed, this, [this] { hideDrives(); });
    m_edit->setPlaceholderText(Gifiles::tr("폴더 경로의 글자를 차례로 치면 바로 찾아갑니다 (예: sigif → Sites/gifiles)"));
    updateKeys();

    connect(m_view, &QListView::doubleClicked, this, [this](const QModelIndex &i) {
        m_view->setCurrentIndex(i);
        choose();
    });
    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &text) {
        // ` typed through an input method (₩ in Korean) arrives as text, not as a key: it closes.
        if (text.contains(QLatin1Char('`')) || text.contains(QChar(0x20A9))) {
            QString t = text;
            t.remove(QLatin1Char('`')).remove(QChar(0x20A9));
            QSignalBlocker block(m_edit);
            m_edit->setText(t);
            m_imeClosed.start();
            return dismiss();
        }
        runQuery(true);
    });
    connect(m_edit, &FolderTreeQuery::queryChanged, this, [this] { runQuery(true); });
    connect(FolderTree::instance(), &FolderTree::indexChanged, this, [this] {
        if (isVisible())
            takeIndex();
    });
    connect(FolderTree::instance(), &FolderTree::progress, this, [this] {
        if (isVisible())
            updateStatus();
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (isVisible() && now && now != this && !isAncestorOf(now))
            dismiss();
    });
    hide();
}

void FolderTreePanel::open(const QString &current)
{
    m_current = current;
    updateKeys();
    {
        QSignalBlocker block(m_edit);
        m_edit->clear();
    }
    m_matches = {};
    m_matchPos = -1;
    hideDrives();
    show();
    raise();
    m_edit->setFocus(Qt::ShortcutFocusReason);
    FolderTree::instance()->acquire(current);
    takeIndex();
}

void FolderTreePanel::dismiss()
{
    if (isHidden())
        return;
    hide();
    hideDrives();
    m_matches = {};
    m_model->setIndex(nullptr);
    m_index.reset();
    FolderTree::instance()->release();
    emit dismissed();
}

QString FolderTreePanel::currentPath() const
{
    const int n = m_model->nodeOf(m_view->currentIndex());
    return m_index && n >= 0 ? m_index->path(n) : QString();
}

void FolderTreePanel::takeIndex()
{
    const QString keep = currentPath().isEmpty() ? m_current : currentPath();
    m_index = FolderTree::instance()->index();
    m_boost = FolderTree::instance()->boost();
    m_model->setIndex(m_index);
    updateKeys();
    if (m_index) {
        // The folder under the cursor or the browser's, or its closest indexed parent; another drive: its top.
        int node = m_index->findNearest(keep);
        if (node < 0)
            node = m_index->findNearest(m_current);
        moveTo(node < 0 && m_index->size() > 0 ? 0 : node);
        runQuery(false);
    }
    updateStatus();
}

void FolderTreePanel::runQuery(bool jump)
{
    const QString q = m_edit->query().trimmed();
    if (!m_index || q.isEmpty()) {
        m_matches = {};
        m_matchPos = -1;
        return updateStatus();
    }
    // Among equal matches the nearest to the folder the browser shows comes first.
    m_matches = m_index->match(q, m_boost, 2000, m_case->isChecked(), m_index->findNearest(m_current));
    const int here = m_model->nodeOf(m_view->currentIndex());
    const auto it = std::find(m_matches.best.begin(), m_matches.best.end(), here);
    m_matchPos = jump ? (m_matches.best.empty() ? -1 : 0) : (it == m_matches.best.end() ? -1 : int(it - m_matches.best.begin()));
    if (jump && m_matchPos == 0)
        jumpTo(m_matches.best[0]);
    updateStatus();
}

void FolderTreePanel::moveTo(int node)
{
    const QModelIndex at = m_model->indexOf(node);
    if (!at.isValid())
        return;
    m_view->setCurrentIndex(at);
    m_view->scrollTo(at, QAbstractItemView::PositionAtCenter);
}

void FolderTreePanel::jumpTo(int node) { moveTo(node); }

void FolderTreePanel::step(int delta)
{
    const int n = int(m_matches.best.size());
    if (n == 0)
        return;
    m_matchPos = m_matchPos < 0 ? (delta > 0 ? 0 : n - 1) : ((m_matchPos + delta) % n + n) % n;
    jumpTo(m_matches.best[size_t(m_matchPos)]);
    updateStatus();
}

void FolderTreePanel::choose()
{
    const QString path = currentPath();
    dismiss();
    if (!path.isEmpty())
        emit chosen(path);
}

void FolderTreePanel::updateKeys()
{
    // The panel's own keys are fixed (not in Settings → 단축키): ⌘ is Ctrl on Windows/Linux.
    auto native = [](const char *portable) { return QKeySequence(QString::fromLatin1(portable)).toString(QKeySequence::NativeText); };
    QList<QPair<QString, QString>> items = {
        {QStringLiteral("Tab"), Gifiles::tr("다음 일치")},
        {QStringLiteral("⇧Tab"), Gifiles::tr("이전 일치")},
        {QStringLiteral("Enter / ") + native("Ctrl+Down"), Gifiles::tr("들어가기")},
        {QStringLiteral("↑ ↓"), Gifiles::tr("한 줄씩")},
        {QStringLiteral("← →"), Gifiles::tr("상위·하위 폴더")},
        {native("Alt+C"), Gifiles::tr("대소문자 구분")},
        {native("Ctrl+D"), Gifiles::tr("드라이브")},
        {native("Ctrl+R"), Gifiles::tr("인덱싱")},
        {QStringLiteral("Esc / `"), Gifiles::tr("닫기")},
    };
    QStringList parts;
    for (const auto &[key, what] : items)
        parts << QStringLiteral("<b>%1</b> %2").arg(key.toHtmlEscaped(), what.toHtmlEscaped());
    m_keys->setText(parts.join(QStringLiteral("&nbsp;&nbsp;·&nbsp;&nbsp;")));
    m_case->setToolTip(Gifiles::tr("대소문자 구분") + QStringLiteral("  ") + native("Alt+C"));
}

void FolderTreePanel::updateStatus()
{
    const QLocale loc;
    const bool scanning = FolderTree::instance()->isScanning();
    QString text;
    if (!m_index)
        text = scanning ? QString() : Gifiles::tr("폴더 목록이 없습니다");
    else if (!m_edit->query().trimmed().isEmpty())
        text = m_matches.best.empty() ? Gifiles::tr("일치하는 폴더 없음")
                                      : QStringLiteral("%1 / %2").arg(m_matchPos < 0 ? QStringLiteral("–") : loc.toString(m_matchPos + 1),
                                                                      loc.toString(m_matches.total));
    else
        text = Gifiles::tr("폴더 %1개").arg(loc.toString(m_index->size() - m_index->rootCount()));
    m_status->setText(text);
    // Indexing (the first scan or ⌘R): a quiet note in the middle of the tree, with the count so far.
    const int done = FolderTree::instance()->scanned();
    m_busy->setText(done > 0 ? Gifiles::tr("인덱싱 중… %1개").arg(loc.toString(done)) : Gifiles::tr("인덱싱 중…"));
    m_busy->setVisible(scanning);
    placeOverlays();
}

void FolderTreePanel::mousePressEvent(QMouseEvent *ev)
{
    // The panel covers the toolbar, so its empty space drags the window instead.
    if (ev->button() == Qt::LeftButton && window()->windowHandle())
        window()->windowHandle()->startSystemMove();
    QFrame::mousePressEvent(ev);
}

void FolderTreePanel::resizeEvent(QResizeEvent *ev)
{
    QFrame::resizeEvent(ev);
    placeOverlays();
}

void FolderTreePanel::placeOverlays()
{
    const QRect area = m_view->geometry();
    if (m_busy->isVisible()) {
        m_busy->adjustSize();
        m_busy->move(area.center() - QPoint(m_busy->width() / 2, m_busy->height() / 2));
        m_busy->raise();
    }
    if (m_drives->isVisible()) {
        const int rows = qMin(m_driveList->count(), 10);
        const int w = qMin(area.width() - 24, 420);
        m_driveList->setFixedHeight(rows * m_driveList->sizeHintForRow(0) + 4);
        m_drives->setFixedWidth(w);
        m_drives->adjustSize();
        m_drives->move(area.left() + (area.width() - w) / 2, area.top() + 24);
        m_drives->raise();
    }
}

void FolderTreePanel::showDrives()
{
    m_driveList->clear();
    const QString current = FolderTree::instance()->drive();
    for (const FolderTree::Drive &d : FolderTree::drives()) {
        auto *item = new QListWidgetItem(d.label, m_driveList);
        item->setData(Qt::UserRole, d.root);
        item->setData(Qt::UserRole + 1, d.path);
        if (d.root == current)
            m_driveList->setCurrentItem(item);
    }
    if (!m_driveList->currentItem() && m_driveList->count() > 0)
        m_driveList->setCurrentRow(0);
    m_drives->show();
    placeOverlays();
}

void FolderTreePanel::hideDrives()
{
    if (m_drives)
        m_drives->hide();
}

void FolderTreePanel::pickDrive()
{
    QListWidgetItem *item = m_driveList->currentItem();
    hideDrives();
    if (item)
        FolderTree::instance()->setDrive(item->data(Qt::UserRole).toString());
}

bool FolderTreePanel::driveKey(QKeyEvent *ke)
{
    const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
    const int k = ke->key();
    const int row = m_driveList->currentRow(), count = m_driveList->count();
    if (k == Qt::Key_Escape || (k == Qt::Key_D && mods == Qt::ControlModifier)) {
        hideDrives();
    } else if (k == Qt::Key_Return || k == Qt::Key_Enter || (k == Qt::Key_Down && mods == Qt::ControlModifier)) {
        pickDrive();
    } else if ((k == Qt::Key_Down || k == Qt::Key_Tab) && count > 0) {
        m_driveList->setCurrentRow((row + 1) % count);
    } else if ((k == Qt::Key_Up || k == Qt::Key_Backtab) && count > 0) {
        m_driveList->setCurrentRow((row - 1 + count) % count);
    } else if (k == Qt::Key_Home && count > 0) {
        m_driveList->setCurrentRow(0);
    } else if (k == Qt::Key_End && count > 0) {
        m_driveList->setCurrentRow(count - 1);
    } else if (const QString t = ke->text().trimmed(); t.size() == 1 && t.at(0).isLetterOrNumber() && count > 0) {
        // A letter: the next drive whose name or path starts with it (D → D:).
        for (int i = 1; i <= count; ++i) {
            QListWidgetItem *item = m_driveList->item((row + i) % count);
            if (item->text().startsWith(t, Qt::CaseInsensitive) || item->data(Qt::UserRole + 1).toString().startsWith(t, Qt::CaseInsensitive)) {
                m_driveList->setCurrentItem(item);
                break;
            }
        }
    }
    return true; // modal: nothing else reaches the panel meanwhile
}

bool FolderTreePanel::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj != m_edit)
        return QFrame::eventFilter(obj, ev);
    const bool drives = m_drives->isVisible();
    if (drives && ev->type() == QEvent::InputMethod)
        return true; // nothing is typed into the query while the drive list is up
    if (ev->type() != QEvent::KeyPress && ev->type() != QEvent::ShortcutOverride)
        return QFrame::eventFilter(obj, ev);
    auto *ke = static_cast<QKeyEvent *>(ev);
    const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
    const int k = ke->key();
    if (ev->type() == QEvent::ShortcutOverride) {
        // The panel's keys win over the menu's while it is open (⌘D is 복제, ⌘↓ 열기); every key while the drive list is up.
        const bool ours = drives || (k == Qt::Key_D && mods == Qt::ControlModifier) || (k == Qt::Key_R && mods == Qt::ControlModifier)
                          || (k == Qt::Key_Down && mods == Qt::ControlModifier) || (k == Qt::Key_C && mods == Qt::AltModifier);
        if (ours)
            ke->accept();
        return ours;
    }
    if (drives)
        return driveKey(ke);
    if (k == Qt::Key_D && mods == Qt::ControlModifier) {
        showDrives();
        return true;
    }
    if (k == Qt::Key_Escape || ((k == Qt::Key_QuoteLeft || k == 0x20A9) && !(mods & ~Qt::ShiftModifier))) {
        dismiss(); // Esc, ` (₩ on the Korean input source)
        return true;
    }
    if (((k == Qt::Key_Return || k == Qt::Key_Enter) && mods == Qt::NoModifier) || (k == Qt::Key_Down && mods == Qt::ControlModifier)) {
        choose(); // Enter, ⌘↓
        return true;
    }
    if (k == Qt::Key_Tab || k == Qt::Key_Backtab) {
        step(k == Qt::Key_Backtab || (mods & Qt::ShiftModifier) ? -1 : 1);
        return true;
    }
    if (k == Qt::Key_R && mods == Qt::ControlModifier) {
        FolderTree::instance()->reindex(); // scans now; the old list stays meanwhile
        updateStatus();
        return true;
    }
    if (k == Qt::Key_C && mods == Qt::AltModifier) {
        m_case->toggle();
        return true;
    }
    const int here = m_model->nodeOf(m_view->currentIndex());
    switch (ke->key()) {
    case Qt::Key_Left: // NCD: ← to the parent, → into the first subfolder
        if (m_index && here >= 0 && m_index->parent(here) >= 0)
            moveTo(m_index->parent(here));
        return true;
    case Qt::Key_Right:
        if (m_index && here >= 0 && m_index->childCount(here) > 0)
            moveTo(m_index->firstChild(here));
        return true;
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        QCoreApplication::sendEvent(m_view, ke);
        return true;
    default:
        return false;
    }
}
