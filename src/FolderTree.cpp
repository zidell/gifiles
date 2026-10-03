#include "FolderTree.h"
#include "App.h"
#include "Permissions.h"
#include "Settings.h"
#include "Shortcuts.h"
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
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QThread>
#include <QListView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
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
        if (m_users == 0 && !m_scanning) {
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
    // A folder made, renamed, moved or trashed in the app: the next panel rescans.
    connect(App::instance(), &App::undoChanged, this, &FolderTree::markStale);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        m_cancel->store(true);
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
    o.roots = Settings::instance()->value(Settings::FolderTreeRoots).toStringList();
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
    const FolderIndex::Options opts = options();
    const QString key = opts.key(), file = cacheFile(key);
    auto counter = m_progress;
    auto cancel = m_cancel;
    auto *watcher = new QFutureWatcher<std::shared_ptr<FolderIndex>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, key] {
        const std::shared_ptr<FolderIndex> result = watcher->result();
        watcher->deleteLater();
        m_scanning = false;
        m_progressTimer.stop();
        const bool current = key == options().key();
        if (result && current) {
            m_index = result;
            m_indexKey = key;
            m_scannedAt = now();
            m_stale = m_staleDuringScan;
            emit indexChanged();
        }
        if (m_users == 0) {
            m_index.reset();
            m_indexKey.clear();
        } else if (m_stale || !current) {
            startScan(false);
        } else {
            emit progress(m_progress->load()); // "새로 읽는 중" ends
        }
    });
    watcher->setFuture(QtConcurrent::run([opts, key, file, counter, cancel, lowPriority]() -> std::shared_ptr<FolderIndex> {
        if (lowPriority)
            QThread::currentThread()->setPriority(QThread::LowestPriority);
        auto idx = std::make_shared<FolderIndex>(FolderIndex::scan(opts, counter.get(), cancel.get()));
        if (lowPriority)
            QThread::currentThread()->setPriority(QThread::NormalPriority);
        if (cancel->load() || idx->rootCount() == 0)
            return {};
        idx->save(file, key, now());
        const QDir dir = QFileInfo(file).absoluteDir(); // caches made for other settings
        for (const QString &old : dir.entryList({QStringLiteral("folder-tree-*.bin")}, QDir::Files))
            if (dir.filePath(old) != file)
                QFile::remove(dir.filePath(old));
        return idx;
    }));
    m_progressTimer.start();
    emit progress(0);
}

void FolderTree::acquire(const QString &folder)
{
    ++m_users;
    m_release.stop();
    const QString key = options().key();
    if (!m_index || m_indexKey != key) {
        FolderIndex idx;
        qint64 at = 0;
        if (FolderIndex::load(cacheFile(key), key, idx, &at)) {
            m_index = std::make_shared<FolderIndex>(std::move(idx));
            m_indexKey = key;
            m_scannedAt = at;
        } else {
            m_index.reset();
            m_indexKey.clear();
            m_scannedAt = 0;
        }
    }
    // A folder's time changes when something inside it is added, renamed or removed.
    const QFileInfo fi(folder);
    if (m_index && fi.exists() && fi.lastModified().toMSecsSinceEpoch() > m_scannedAt)
        m_stale = true;
    if (!m_index || m_stale || now() - m_scannedAt > kMaxAgeOpen)
        startScan(false);
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
        if (m_scanning)
            return;
        const QFileInfo cache(cacheFile(options().key()));
        if (m_stale || !cache.exists() || cache.lastModified().msecsTo(QDateTime::currentDateTime()) > kMaxAgeIdle)
            startScan(true);
    };
    QTimer::singleShot(20 * 1000, this, check); // after the windows have settled
    m_prefetch.setInterval(60 * 60 * 1000);
    connect(&m_prefetch, &QTimer::timeout, this, check);
    m_prefetch.start();
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
            p->setBrush(selected ? c.selection : c.hover);
            p->drawRoundedRect(r.adjusted(2, 0, -2, -1), 5, 5);
        }
        auto isLast = [f](int node) {
            const int parent = f->parent(node);
            return parent < 0 ? node == f->rootCount() - 1 : node == f->firstChild(parent) + f->childCount(parent) - 1;
        };
        // Lines: one per open ancestor level, then this folder's own branch.
        const int depth = f->depth(n);
        p->setRenderHint(QPainter::Antialiasing, false);
        p->setPen(QPen(selected ? c.selText : c.tertiary, 1));
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
        const QColor iconColor = selected ? c.selText : c.secondary;
        Theme::icon(QStringLiteral("folder"), iconColor, 16).paint(p, QRect(iconX, midY - 8, 16, 16));
        const QRect textRect(iconX + 22, r.top(), r.right() - iconX - 26, r.height());
        p->setPen(selected ? c.selText : c.text);
        p->setFont(option.font);
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                    option.fontMetrics.elidedText(index.data().toString(), Qt::ElideMiddle, textRect.width()));
        p->restore();
    }

private:
    FolderTreeModel *m_model;
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
    m_edit = new FolderTreeQuery(this);
    m_edit->setObjectName(QStringLiteral("folderTreeQuery"));
    m_edit->setAttribute(Qt::WA_InputMethodEnabled, true);
    m_edit->setPlaceholderText(Gifiles::tr("폴더 이름을 치면 바로 찾아갑니다 · Tab 다음 · Enter 이동 · Esc 닫기"));
    m_edit->installEventFilter(this);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("secondary"));
    row->addWidget(m_edit, 1);
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

    connect(m_view, &QListView::doubleClicked, this, [this](const QModelIndex &i) {
        m_view->setCurrentIndex(i);
        choose();
    });
    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &text) {
        // ` typed through an input method (₩ in Korean) arrives as text, not as a key.
        if (text.contains(QLatin1Char('`')) || text.contains(QChar(0x20A9))) {
            const QStringList keys = Shortcuts::toStrings(Shortcuts::instance()->keys(QStringLiteral("폴더 트리 닫기")));
            if (keys.contains(QStringLiteral("`"))) {
                QString t = text;
                t.remove(QLatin1Char('`')).remove(QChar(0x20A9));
                QSignalBlocker block(m_edit);
                m_edit->setText(t);
                m_imeClosed.start();
                return dismiss();
            }
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
    {
        QSignalBlocker block(m_edit);
        m_edit->clear();
    }
    m_matches = {};
    m_matchPos = -1;
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
    if (m_index) {
        moveTo(m_index->findNearest(keep)); // the folder the browser shows, or its closest indexed parent
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
    m_matches = m_index->match(q, m_boost);
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

void FolderTreePanel::updateStatus()
{
    const QLocale loc;
    const bool scanning = FolderTree::instance()->isScanning();
    QString text;
    if (!m_index)
        text = scanning ? Gifiles::tr("폴더 목록을 만드는 중… %1개").arg(loc.toString(FolderTree::instance()->scanned()))
                        : Gifiles::tr("폴더 목록이 없습니다");
    else if (!m_edit->query().trimmed().isEmpty())
        text = m_matches.best.empty() ? Gifiles::tr("일치하는 폴더 없음")
                                      : QStringLiteral("%1 / %2").arg(m_matchPos < 0 ? QStringLiteral("–") : loc.toString(m_matchPos + 1),
                                                                      loc.toString(m_matches.total));
    else
        text = Gifiles::tr("폴더 %1개").arg(loc.toString(m_index->size() - m_index->rootCount()));
    if (m_index && scanning)
        text += QStringLiteral(" · ") + Gifiles::tr("새로 읽는 중");
    m_status->setText(text);
}

bool FolderTreePanel::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj != m_edit || ev->type() != QEvent::KeyPress)
        return QFrame::eventFilter(obj, ev);
    auto *ke = static_cast<QKeyEvent *>(ev);
    // ⇧Tab arrives as Backtab; the keys are given as Shift+Tab.
    const bool backtab = ke->key() == Qt::Key_Backtab;
    const QKeyEvent key(ke->type(), backtab ? Qt::Key_Tab : ke->key(), ke->modifiers() | (backtab ? Qt::ShiftModifier : Qt::NoModifier),
                        ke->text());
    const auto matches = [&key](const char *id) { return Shortcuts::instance()->matches(QString::fromUtf8(id), &key); };
    if (matches("폴더 트리 닫기")) {
        dismiss();
        return true;
    }
    if (matches("폴더 트리에서 이동")) {
        choose();
        return true;
    }
    if (matches("폴더 트리 다음 일치") || matches("폴더 트리 이전 일치")) {
        step(matches("폴더 트리 다음 일치") ? 1 : -1);
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
