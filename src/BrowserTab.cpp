#include "BrowserTab.h"
#include "FileOps.h"
#include "FileProxy.h"
#include "ItemDelegate.h"
#include "OpenWith.h"
#include "Preview.h"
#include "Settings.h"
#include "Shortcuts.h"
#include "Theme.h"
#include "Thumbnails.h"
#include "Util.h"

#include <QApplication>
#include <QLineEdit>
#include <QColumnView>
#include <QCryptographicHash>
#include <QSettings>
#include <QStandardPaths>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QKeyEvent>
#include <QListView>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QTimer>
#include <QPainter>
#include <QPainterPath>
#include <QRubberBand>
#include <QTreeView>
#include <QVBoxLayout>

#include <functional>

namespace {
constexpr int kMacTextWeight = QFont::Light; // see applyFont

// Finder-style list: pressing on a row's name (icon + text) drags the file; pressing anywhere
// else (other columns, the empty end of a row, below the rows) draws a selection rectangle.
class FileTreeView : public QTreeView {
public:
    using QTreeView::QTreeView;

    void setStripedRows(bool enabled)
    {
        setProperty("stripedRows", enabled);
        // drawRow owns every background. Native alternating fills can cover its selection pill.
        setAlternatingRowColors(false);
        viewport()->update();
    }

    bool isOnName(const QPoint &pos) const
    {
        const QModelIndex idx = indexAt(pos);
        if (!idx.isValid() || idx.column() != 0)
            return false;
        const QRect r = visualRect(idx);
        const int textWidth = fontMetrics().horizontalAdvance(idx.data().toString());
        return pos.x() >= r.left() && pos.x() <= r.left() + iconSize().width() + textWidth + 24;
    }

protected:
    // Stripes are painted here as full-width rounded rows, so folder rows (with a disclosure
    // chevron) and file rows look the same.
    void drawRow(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        // QTreeView decides odd/even internally and doesn't pass it in; derive it from the row's position.
        const int rowH = qMax(1, option.rect.height());
        const bool odd = ((option.rect.top() + verticalOffset()) / rowH) % 2 == 1;
        // Selection and hover are painted here too, as one pill under the whole row (chevron
        // included); per-cell backgrounds would split at the branch area.
        const Theme::Colors &c = Theme::colors();
        QColor fill;
        if (selectionModel() && selectionModel()->isSelected(index)) {
            const auto *d = qobject_cast<ItemDelegate *>(itemDelegate());
            const QColor own = d && (option.state & QStyle::State_Active) ? d->selectionColor(index) : QColor();
            fill = own.isValid() ? own : (option.state & QStyle::State_Active) ? c.selection : c.selInactive;
        }
        else if (const QModelIndex h = indexAt(viewport()->mapFromGlobal(QCursor::pos()));
                 h.isValid() && h.row() == index.row() && h.parent() == index.parent() && viewport()->underMouse())
            fill = c.hover;
        else if (property("stripedRows").toBool() && odd)
            fill = c.altRow;
        if (fill.isValid()) {
            QPainterPath path;
            // 1px gap below each pill, so adjacent selected rows still read as separate rows.
            path.addRoundedRect(QRectF(option.rect).adjusted(2, 0, -2, -1), 7, 7);
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->fillPath(path, fill);
            p->restore();
        }
        QStyleOptionViewItem opt(option);
        opt.state &= ~QStyle::State_HasFocus; // no focus frame around the current row
        QTreeView::drawRow(p, opt, index);
    }

    // A selected folder row: the style's white "selected" chevron would vanish on a light bar, so the
    // chevron is drawn here, black on the item's own color or in the text color on the shared bar.
    void drawBranches(QPainter *p, const QRect &rect, const QModelIndex &index) const override
    {
        const auto *d = qobject_cast<ItemDelegate *>(itemDelegate());
        if (!d || !selectionModel() || !selectionModel()->isSelected(index) || !window()->isActiveWindow() ||
            !d->selectionColor(index).isValid()) {
            QTreeView::drawBranches(p, rect, index);
            return;
        }
        if (!model()->hasChildren(index))
            return;
        const QRect r(rect.right() - indentation() + 1, rect.top(), indentation(), rect.height());
        Theme::icon(isExpanded(index) ? QStringLiteral("chevron-down-small") : QStringLiteral("chevron-right-small"),
                    Settings::instance()->flag(Settings::FileColorSelection) ? Theme::selectionTextColor() : Theme::colors().text, 16)
            .paint(p, r);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const QPoint pos = e->position().toPoint();
        m_banding = e->button() == Qt::LeftButton && !isOnName(pos);
        // Without drag enabled, QTreeView turns press-and-move into a range selection.
        setDragEnabled(!m_banding);
        if (m_banding) {
            m_origin = pos + QPoint(horizontalOffset(), verticalOffset());
            if (!m_band)
                m_band = new QRubberBand(QRubberBand::Rectangle, viewport());
        }
        QTreeView::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_banding && (e->buttons() & Qt::LeftButton)) {
            const QPoint origin = m_origin - QPoint(horizontalOffset(), verticalOffset());
            m_band->setGeometry(QRect(origin, e->position().toPoint()).normalized());
            m_band->show();
        }
        QTreeView::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (m_band)
            m_band->hide();
        m_banding = false;
        setDragEnabled(true);
        QTreeView::mouseReleaseEvent(e);
    }

private:
    bool m_banding = false;
    QPoint m_origin; // in content coordinates, so the band follows auto-scroll
    QRubberBand *m_band = nullptr;
};

// QColumnView builds one inner view per column; we need to hook keys/context menus on each.
class ColumnView : public QColumnView {
public:
    using QColumnView::QColumnView;
    std::function<void(QAbstractItemView *)> onColumn;

protected:
    QAbstractItemView *createColumn(const QModelIndex &rootIndex) override
    {
        QAbstractItemView *v = QColumnView::createColumn(rootIndex);
        if (onColumn)
            onColumn(v);
        return v;
    }

};

// Per-folder view settings live in their own file, keyed by the folder's real path.
QSettings &folderStore()
{
    static QSettings store(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/folders.ini"),
                           QSettings::IniFormat);
    return store;
}

QString folderKey(const QString &path)
{
    QString canon = QFileInfo(path).canonicalFilePath();
    if (canon.isEmpty())
        canon = QDir::cleanPath(path);
    return QString::fromLatin1(QCryptographicHash::hash(canon.toUtf8(), QCryptographicHash::Sha1).toHex());
}

constexpr QDir::Filters kBaseFilter = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs | QDir::System;

void configureDnD(QAbstractItemView *v)
{
    // Pixel scrolling follows trackpad/Magic Mouse deltas like native macOS lists; per-item
    // scrolling jumps whole rows per tiny gesture and feels far too fast.
    v->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    v->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    v->setMouseTracking(true); // hover highlight
    v->viewport()->setAttribute(Qt::WA_Hover);
    v->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->setDragEnabled(true);
    v->setAcceptDrops(true);
    v->setDropIndicatorShown(true);
    v->setDragDropMode(QAbstractItemView::DragDrop);
    v->setDefaultDropAction(Qt::MoveAction);
    v->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    v->setAttribute(Qt::WA_MacShowFocusRect, false);
    v->setContextMenuPolicy(Qt::CustomContextMenu);
}

void setFinderColumnOrder(QHeaderView *header)
{
    // Keep the same Finder-style order on every platform, even for folders that saved the old layout.
    for (int logical = ColName; logical <= ColDate; ++logical)
        if (const int visual = header->visualIndex(logical); visual != logical)
            header->moveSection(visual, logical);
}

} // namespace

BrowserTab::BrowserTab(const QString &path, Mode mode, bool showHidden, QWidget *parent) : QWidget(parent)
{
    m_fs = new QFileSystemModel(this);
    m_fs->setReadOnly(false); // enables in-place editing; the delegate performs the actual rename
    m_fs->setFilter(kBaseFilter | (showHidden ? QDir::Hidden : QDir::Filters()));
    m_fs->setResolveSymlinks(true);
    m_proxy = new FileProxy(m_fs, this);
    m_proxy->sort(ColName, Qt::AscendingOrder);

    m_listDelegate = new ItemDelegate(m_proxy, ItemDelegate::List, this);
    m_galleryDelegate = new ItemDelegate(m_proxy, ItemDelegate::Gallery, this);
    m_columnDelegate = new ItemDelegate(m_proxy, ItemDelegate::Column, this);
    for (ItemDelegate *d : {m_listDelegate, m_galleryDelegate, m_columnDelegate})
        connect(d, &ItemDelegate::renameRequested, this, &BrowserTab::renameRequested);

    // List
    auto *list = new FileTreeView(this);
    m_list = list;
    m_list->setModel(m_proxy);
    m_list->setItemDelegate(m_listDelegate);
    // Folders expand in place (→ / ←, or the disclosure chevron); ⌘↓ or double-click enters them.
    m_list->setRootIsDecorated(true);
    m_list->setItemsExpandable(true);
    m_list->setIndentation(18);
    m_list->setTextElideMode(Qt::ElideMiddle); // long names keep their end (extension, numbers)
    m_list->setAnimated(true);
    m_list->setExpandsOnDoubleClick(false);
    m_list->setUniformRowHeights(true);
    m_list->setAllColumnsShowFocus(false); // otherwise QTreeView frames the whole current row
    static_cast<FileTreeView *>(m_list)->setStripedRows(Settings::instance()->flag(Settings::Stripes));
    connect(Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String(Settings::Stripes))
            static_cast<FileTreeView *>(m_list)->setStripedRows(Settings::instance()->flag(Settings::Stripes));
        if (key == QLatin1String(Settings::FoldersFirst))
            m_proxy->invalidate();
    });
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(ColName, Qt::AscendingOrder);
    m_list->setIconSize(QSize(18, 18));
    m_list->setObjectName(QStringLiteral("files"));
    m_list->setFrameShape(QFrame::NoFrame);
    configureDnD(m_list);
    QHeaderView *h = m_list->header();
    setFinderColumnOrder(h); // 이름 · 크기 · 종류 · 수정일
    h->setStretchLastSection(true);
    h->setSectionsMovable(true);
    m_list->setColumnWidth(ColName, 320);
    m_list->setColumnWidth(ColDate, 150);
    m_list->setColumnWidth(ColSize, 90);
    // Sorting or resizing/reordering columns by hand is remembered for the folder.
    m_prefsTimer = new QTimer(this);
    m_prefsTimer->setSingleShot(true);
    m_prefsTimer->setInterval(300);
    connect(m_prefsTimer, &QTimer::timeout, this, &BrowserTab::saveFolderPrefs);
    auto userChange = [this] {
        if (!m_applyingPrefs && (QApplication::mouseButtons() & Qt::LeftButton))
            m_prefsTimer->start();
    };
    connect(h, &QHeaderView::sectionResized, this, userChange);
    connect(h, &QHeaderView::sectionMoved, this, userChange);
    connect(h, &QHeaderView::sortIndicatorChanged, this, [this] {
        if (!m_applyingPrefs && !m_path.isEmpty())
            m_prefsTimer->start();
    });

    // Gallery
    m_gallery = new QListView(this);
    m_gallery->setModel(m_proxy);
    QItemSelectionModel *own = m_gallery->selectionModel();
    m_gallery->setSelectionModel(m_list->selectionModel()); // list and gallery share one selection
    delete own;
    m_gallery->setItemDelegate(m_galleryDelegate);
    m_gallery->setViewMode(QListView::IconMode);
    m_gallery->setResizeMode(QListView::Adjust);
    m_gallery->setMovement(QListView::Static);
    m_gallery->setWrapping(true);
    m_gallery->setUniformItemSizes(true);
    m_gallery->setWordWrap(true);
    m_gallery->setTextElideMode(Qt::ElideMiddle);
    m_gallery->setSelectionRectVisible(true);
    m_gallery->setFrameShape(QFrame::NoFrame);
    m_gallery->setObjectName(QStringLiteral("files"));
    m_gallery->setSpacing(4);
    configureDnD(m_gallery);
    setIconSize(m_iconSize);
    connect(Thumbnails::instance(), &Thumbnails::ready, m_gallery->viewport(), [this] {
        if (m_mode == Gallery)
            m_gallery->viewport()->update();
    });

    // Columns
    auto *cols = new ColumnView(this);
    m_columns = cols;
    m_columns->setModel(m_proxy);
    m_columns->setTextElideMode(Qt::ElideMiddle); // copied to each column it creates
    m_columns->setItemDelegate(m_columnDelegate);
    m_columns->setFrameShape(QFrame::NoFrame);
    m_columns->setObjectName(QStringLiteral("files"));
    m_columns->setResizeGripsVisible(false);
    m_columns->setIconSize(QSize(18, 18));
    configureDnD(m_columns);
    m_columnPreview = new PreviewWidget(PreviewWidget::Pane);
    m_columnPreview->setMinimumWidth(260);
    m_columns->setPreviewWidget(m_columnPreview);
    cols->onColumn = [this](QAbstractItemView *v) { attachView(v); };

    // The views' text size (⌘+ / ⌘−); rows and tiles follow the font's height.
    auto applyFont = [this] {
        QFont f = QApplication::font();
        if (const int pt = Settings::instance()->value(Settings::FontSize).toInt(); pt > 0)
            f.setPointSize(pt);
#ifdef Q_OS_MACOS
        // Qt draws text with macOS's grayscale smoothing (stem darkening) always on, so the regular
        // weight looks semi-bold beside Finder's; the system font is variable: a little lighter.
        f.setWeight(QFont::Weight(kMacTextWeight));
#endif
        for (QAbstractItemView *v : {static_cast<QAbstractItemView *>(m_list), static_cast<QAbstractItemView *>(m_gallery),
                                     static_cast<QAbstractItemView *>(m_columns)}) {
            v->setFont(f);
            v->doItemsLayout();
        }
    };
    applyFont();
    connect(Settings::instance(), &Settings::changed, this, [applyFont](const QString &key) {
        if (key == QLatin1String(Settings::FontSize))
            applyFont();
    });
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_list);
    m_stack->addWidget(m_gallery);
    m_stack->addWidget(m_columns);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(m_stack);

    for (QAbstractItemView *v : {static_cast<QAbstractItemView *>(m_list), static_cast<QAbstractItemView *>(m_gallery),
                                 static_cast<QAbstractItemView *>(m_columns)})
        attachView(v);

    auto onSel = [this] { emit selectionChanged(); };
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this, onSel);
    connect(m_columns->selectionModel(), &QItemSelectionModel::selectionChanged, this, onSel);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &c) { if (m_mode != Columns) onCurrentChanged(c); });
    connect(m_columns->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &c) { if (m_mode == Columns) onCurrentChanged(c); });

    connect(m_proxy, &FileProxy::dropRequested, this, &BrowserTab::dropRequested, Qt::QueuedConnection);
    auto retry = [this] {
        if (!m_pending.isEmpty() || !m_enterWhenLoaded.isEmpty())
            QTimer::singleShot(0, this, &BrowserTab::trySelectPending);
    };
    connect(m_fs, &QFileSystemModel::directoryLoaded, this, retry);
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, retry);
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, [this] {
        if (m_justSelected.isEmpty() || !m_justSelectedAt.isValid() || m_justSelectedAt.elapsed() > 2000) {
            m_justSelected.clear();
            return;
        }
        if (m_pending.isEmpty() && selectedPaths().isEmpty())
            QTimer::singleShot(0, this, [this] {
                if (!m_pending.isEmpty() || !selectedPaths().isEmpty() || m_justSelected.isEmpty())
                    return;
                // A rename editor that closed with the row reopens with what was typed.
                ItemDelegate *d = m_mode == Gallery ? m_galleryDelegate : m_mode == Columns ? m_columnDelegate : m_listDelegate;
                const std::optional<QString> text = m_justSelected.size() == 1 ? d->takeLostEdit(m_justSelected.first()) : std::nullopt;
                selectPaths(m_justSelected, text.has_value());
                if (text)
                    if (auto *edit = qobject_cast<QLineEdit *>(QApplication::focusWidget()); edit && edit->objectName() == QLatin1String("renameEdit"))
                        edit->setText(*text);
            });
    });
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, [this] { emit selectionChanged(); });
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, [this] { emit selectionChanged(); });
    // Without its root row the list would show the whole disk from "/".
    m_rootGone = new QTimer(this);
    m_rootGone->setSingleShot(true);
    m_rootGone->setInterval(1500);
    connect(m_rootGone, &QTimer::timeout, this, &BrowserTab::checkRoot);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, [this] { QTimer::singleShot(0, this, &BrowserTab::checkRoot); });
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, [this] { QTimer::singleShot(0, this, &BrowserTab::checkRoot); });
    connect(m_fs, &QFileSystemModel::directoryLoaded, this, [this] { QTimer::singleShot(0, this, &BrowserTab::checkRoot); });

    m_mode = mode;
    m_stack->setCurrentIndex(mode);
    navigate(path);
    qApp->installEventFilter(this);
}

void BrowserTab::attachView(QAbstractItemView *v)
{
    v->installEventFilter(this);
    v->setMouseTracking(true);
    v->viewport()->setAttribute(Qt::WA_Hover);
    v->setIconSize(QSize(18, 18));
    v->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    v->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    v->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(v, &QWidget::customContextMenuRequested, this, [this, v](const QPoint &pos) {
        const QModelIndex idx = v->indexAt(pos);
        QItemSelectionModel *sm = v->selectionModel();
        if (idx.isValid() && !sm->isSelected(idx)) {
            sm->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            sm->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
        } else if (!idx.isValid() && m_mode != Columns) {
            sm->clearSelection();
        }
        emit contextMenuRequested(v->viewport()->mapToGlobal(pos), idx.isValid());
    });
    // QColumnView re-emits doubleClicked from its inner columns, so only hook top-level views.
    if (v->parentWidget() == m_columns->viewport())
        return;
    connect(v, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &idx) {
        openIndex(idx, QApplication::keyboardModifiers() & Qt::ControlModifier);
    });
}

void BrowserTab::setMode(Mode mode, bool remember)
{
    if (mode == m_mode) {
        if (remember)
            saveFolderPrefs();
        return;
    }
    const QStringList sel = selectedPaths();
    m_mode = mode;
    m_stack->setCurrentIndex(mode);
    if (mode == Columns) {
        syncColumns();
        if (!sel.isEmpty())
            selectPaths(sel);
    } else {
        applyRoot();
        QStringList inside;
        for (const QString &p : sel)
            if (QFileInfo(p).absolutePath() == m_path)
                inside << p;
        if (!inside.isEmpty())
            selectPaths(inside);
    }
    focusView();
    emit selectionChanged();
    if (remember)
        saveFolderPrefs();
}

QAbstractItemView *BrowserTab::view() const
{
    switch (m_mode) {
    case Gallery: return m_gallery;
    case Columns: return m_columns;
    default: return m_list;
    }
}

void BrowserTab::focusView(bool selectFirstIfEmpty)
{
    QAbstractItemView *v = view();
    if (m_mode == Columns)
        if (QAbstractItemView *c = columnFor(m_columns->currentIndex().parent()); c && c->isVisible())
            v = c;
    v->setFocus();
    if (selectFirstIfEmpty && selectedPaths().isEmpty()) {
        const QModelIndex first = v->model()->index(0, 0, v->rootIndex());
        if (first.isValid())
            selectIndexes({first});
    }
}

void BrowserTab::navigate(const QString &rawPath, bool pushHistory, const QStringList &select)
{
    QFileInfo fi(rawPath);
    QString path = QDir::cleanPath(fi.absoluteFilePath());
    QStringList sel = select;
    if (fi.exists() && !fi.isDir()) {
        sel = {path};
        path = fi.absolutePath();
    }
    if (!QFileInfo(path).isDir())
        return;
    m_pending.clear();
    m_pendingRename = false;
    m_enterWhenLoaded.clear();
    const bool changed = path != m_path;
    setPathInternal(path, pushHistory);
    if (changed)
        applyFolderPrefs();
    if (m_mode == Columns)
        syncColumns();
    else
        applyRoot();
    if (!sel.isEmpty())
        selectPaths(sel);
    else if (m_mode != Columns)
        m_list->selectionModel()->clearSelection();
}

void BrowserTab::setPathInternal(const QString &path, bool pushHistory)
{
    const bool changed = path != m_path;
    if (changed && m_prefsTimer && m_prefsTimer->isActive()) {
        // A view change still waiting to be saved belongs to the folder we are leaving.
        m_prefsTimer->stop();
        saveFolderPrefs();
    }
    m_path = path;
    m_fs->setRootPath(path);
    m_proxy->setSearchRoot(path);
    if (pushHistory && changed) {
        m_history = m_history.mid(0, m_histIndex + 1);
        m_history << path;
        m_histIndex = m_history.size() - 1;
    }
    if (changed)
        emit pathChanged(path);
}

void BrowserTab::applyRoot()
{
    m_list->collapseAll();
    const QModelIndex root = m_proxy->indexForPath(m_path);
    m_list->setRootIndex(root);
    m_gallery->setRootIndex(root);
    m_list->scrollToTop();
    m_gallery->scrollToTop();
}

void BrowserTab::checkRoot()
{
    if (m_path.isEmpty())
        return;
    const QModelIndex root = m_list->rootIndex();
    if (root.isValid() && QDir::cleanPath(m_proxy->filePath(root)) == QDir::cleanPath(m_path)) {
        m_rootGone->stop();
        return;
    }
    if (QFileInfo(m_path).isDir()) { // back again (renamed away and back, replaced by a build tool)
        m_rootGone->stop();
        m_fs->setRootPath(m_path);
        const QModelIndex again = m_proxy->indexForPath(m_path);
        if (again.isValid()) {
            m_list->setRootIndex(again);
            m_gallery->setRootIndex(again);
        }
        return;
    }
    // Gone: show the parent meanwhile, and go there if it doesn't come back.
    QString parent = QFileInfo(m_path).absolutePath();
    while (!QFileInfo(parent).isDir() && parent != QDir::rootPath() && !parent.isEmpty())
        parent = QFileInfo(parent).absolutePath();
    if (!m_rootGone->isActive() && root.isValid() && QDir::cleanPath(m_proxy->filePath(root)) == QDir::cleanPath(parent)) {
        navigate(parent); // the parent shown meanwhile, and the timer has run out
        return;
    }
    if (const QModelIndex up = m_proxy->indexForPath(parent); up.isValid() && root != up) {
        m_list->setRootIndex(up);
        m_gallery->setRootIndex(up);
    }
    if (!m_rootGone->isActive())
        m_rootGone->start();
}

void BrowserTab::syncColumns()
{
    m_syncingColumns = true;
    QString volume = QStorageInfo(m_path).rootPath();
    if (volume.isEmpty())
        volume = QDir::rootPath();
    const QModelIndex root = m_proxy->indexForPath(volume);
    if (m_columns->rootIndex() != root)
        m_columns->setRootIndex(root);
    const QModelIndex cur = m_proxy->indexForPath(m_path);
    if (m_path != volume && cur.isValid()) {
        m_columns->selectionModel()->setCurrentIndex(cur, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_columns->scrollTo(cur);
    } else {
        m_columns->selectionModel()->clearSelection();
    }
    m_syncingColumns = false;
}

void BrowserTab::onCurrentChanged(const QModelIndex &current)
{
    const QString p = m_proxy->filePath(current);
    if (m_mode == Columns && !m_syncingColumns && current.isValid()) {
        // In column view the "current folder" follows the selection: a selected folder, or the
        // folder containing the selected file.
        const QFileInfo fi(p);
        const QString dir = fi.isDir() && !util::isPackage(fi) ? p : fi.absolutePath();
        if (dir != m_path)
            setPathInternal(dir, false);
        if (!fi.isDir() || util::isPackage(fi))
            m_columnPreview->setPath(p);
    }
    emit currentItemChanged(p);
}

void BrowserTab::goBack()
{
    if (!canGoBack())
        return;
    const QString from = m_path;
    --m_histIndex;
    navigate(m_history[m_histIndex], false);
    if (util::isInside(from, m_path) && from != m_path) {
        QString child = from;
        while (QFileInfo(child).absolutePath() != m_path)
            child = QFileInfo(child).absolutePath();
        selectPaths({child});
    }
}

void BrowserTab::goForward()
{
    if (!canGoForward())
        return;
    ++m_histIndex;
    navigate(m_history[m_histIndex], false);
}

void BrowserTab::goUp()
{
    QDir d(m_path);
    if (!d.cdUp())
        return;
    navigate(d.absolutePath(), true, {m_path});
}

QStringList BrowserTab::selectedPaths() const
{
    QStringList out;
    const QAbstractItemView *v = view();
    QModelIndexList idxs = v->selectionModel()->selectedIndexes();
    // Keep display order.
    std::sort(idxs.begin(), idxs.end(), [](const QModelIndex &a, const QModelIndex &b) {
        return a.parent() == b.parent() ? a.row() < b.row() : a.parent().row() < b.parent().row();
    });
    for (const QModelIndex &i : idxs)
        if (i.column() == 0) {
            const QString p = m_proxy->filePath(i);
            if (!out.contains(p))
                out << p;
        }
    return out;
}

QString BrowserTab::currentItemPath() const
{
    const QModelIndex c = view()->currentIndex();
    if (c.isValid() && view()->selectionModel()->isSelected(c))
        return m_proxy->filePath(c);
    const QStringList sel = selectedPaths();
    return sel.isEmpty() ? QString() : sel.first();
}

QString BrowserTab::neighborOfSelection() const
{
    QItemSelectionModel *sm = view()->selectionModel();
    QModelIndexList rows;
    for (const QModelIndex &i : sm->selectedIndexes())
        if (i.column() == 0)
            rows << i;
    if (rows.isEmpty())
        return {};
    const QModelIndex parent = rows.first().parent();
    int lo = rows.first().row(), hi = lo;
    for (const QModelIndex &i : rows) {
        lo = qMin(lo, i.row());
        hi = qMax(hi, i.row());
    }
    const int n = m_proxy->rowCount(parent);
    for (int r = hi + 1; r < n; ++r)
        if (!sm->isSelected(m_proxy->index(r, 0, parent)))
            return m_proxy->filePath(m_proxy->index(r, 0, parent));
    for (int r = lo - 1; r >= 0; --r)
        if (!sm->isSelected(m_proxy->index(r, 0, parent)))
            return m_proxy->filePath(m_proxy->index(r, 0, parent));
    return {};
}

void BrowserTab::selectPaths(const QStringList &paths, bool thenRename)
{
    m_pending = paths;
    m_pendingRename = thenRename;
    trySelectPending();
}

void BrowserTab::selectIndexes(const QModelIndexList &idxs)
{
    if (idxs.isEmpty())
        return;
    QItemSelectionModel *sm = view()->selectionModel();
    QItemSelection s;
    for (const QModelIndex &i : idxs)
        s.select(i, i);
    sm->select(s, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    sm->setCurrentIndex(idxs.first(), QItemSelectionModel::NoUpdate);
    if (m_mode == Columns) {
        sm->setCurrentIndex(idxs.first(), QItemSelectionModel::Current);
        m_columns->scrollTo(idxs.first());
    } else {
        view()->scrollTo(idxs.first());
    }
}

void BrowserTab::trySelectPending()
{
    if (!m_enterWhenLoaded.isEmpty() && m_mode == Columns) {
        const QModelIndex dir = m_proxy->indexForPath(m_enterWhenLoaded);
        const QModelIndex child = m_proxy->index(0, 0, dir);
        if (child.isValid()) {
            m_enterWhenLoaded.clear();
            selectIndexes({child});
            if (QAbstractItemView *c = columnFor(dir))
                c->setFocus();
        }
    }
    if (m_pending.isEmpty())
        return;
    QModelIndexList found;
    for (const QString &p : m_pending) {
        if (!util::exists(p))
            continue;
        const QModelIndex idx = m_proxy->indexForPath(p);
        if (!idx.isValid())
            return; // not loaded yet; retried on rowsInserted/directoryLoaded
        found << idx;
    }
    m_justSelected.clear();
    for (const QModelIndex &i : found)
        m_justSelected << m_proxy->filePath(i);
    m_justSelectedAt.start();
    m_pending.clear();
    selectIndexes(found);
    if (m_pendingRename && found.size() == 1) {
        m_pendingRename = false;
        renameSelected();
    }
}

void BrowserTab::selectAll()
{
    if (m_mode == Columns) {
        if (QAbstractItemView *c = columnFor(m_columns->currentIndex().parent()))
            c->selectAll();
        return;
    }
    view()->selectAll();
}

QAbstractItemView *BrowserTab::columnFor(const QModelIndex &parent) const
{
    // QColumnView keeps recycled hidden columns. Prefer the one actually being displayed.
    QAbstractItemView *hidden = nullptr;
    const auto views = m_columns->viewport()->findChildren<QAbstractItemView *>();
    for (QAbstractItemView *v : views)
        if (v->rootIndex() == parent) {
            if (v->isVisible())
                return v;
            if (!hidden)
                hidden = v;
        }
    return hidden;
}

void BrowserTab::renameSelected()
{
    QModelIndexList idxs;
    for (const QModelIndex &i : view()->selectionModel()->selectedIndexes())
        if (i.column() == 0)
            idxs << i;
    if (idxs.size() != 1)
        return;
    const QModelIndex idx = idxs.first();
    if (m_mode == Columns) {
        if (QAbstractItemView *c = columnFor(idx.parent())) {
            c->setCurrentIndex(idx);
            c->edit(idx);
        }
        return;
    }
    view()->scrollTo(idx);
    view()->edit(idx);
}

void BrowserTab::openIndex(const QModelIndex &idx, bool inNewTab)
{
    if (!idx.isValid())
        return;
    const QString p = m_proxy->filePath(idx);
    const QFileInfo fi(p);
    if (fi.isDir() && !util::isPackage(fi)) {
        if (inNewTab) {
            emit openInNewTabRequested(p);
        } else if (m_mode == Columns) {
            // Step into the folder: select its first item (once loaded, if it isn't yet).
            m_enterWhenLoaded = p;
            if (m_proxy->canFetchMore(idx))
                m_proxy->fetchMore(idx);
            trySelectPending();
        } else {
            navigate(p);
        }
        return;
    }
    if (fi.isDir())
        QDesktopServices::openUrl(QUrl::fromLocalFile(p)); // app bundles
    else if (FileOps::isArchive(p) && !OpenWith::hasRememberedApp(p))
        emit extractRequested({p}); // expanded here, not in Finder
    else
        OpenWith::open(p); // honours "항상 이 앱으로 열기"
}

void BrowserTab::openSelection(bool inNewTab)
{
    QModelIndexList idxs;
    for (const QModelIndex &i : view()->selectionModel()->selectedIndexes())
        if (i.column() == 0)
            idxs << i;
    bool navigated = false;
    for (const QModelIndex &i : idxs) {
        const QFileInfo fi(m_proxy->filePath(i));
        if (fi.isDir() && !util::isPackage(fi)) {
            // The first folder opens here (unless asked for a tab); any others open in new tabs.
            openIndex(i, inNewTab || navigated);
            navigated = true;
        } else if (!inNewTab) { // files have no tab to open in
            openIndex(i, false);
        }
    }
}

bool BrowserTab::hasSelectedFolder() const
{
    const QStringList sel = selectedPaths();
    return std::any_of(sel.begin(), sel.end(), [](const QString &p) {
        const QFileInfo fi(p);
        return fi.isDir() && !util::isPackage(fi);
    });
}

QPoint BrowserTab::selectionMenuPos() const
{
    QAbstractItemView *v = view();
    QModelIndex cur = v->currentIndex();
    if (m_mode == Columns)
        if (QAbstractItemView *c = columnFor(cur.parent()))
            v = c;
    if (!v->selectionModel()->isSelected(cur) && !v->selectionModel()->selectedIndexes().isEmpty())
        cur = v->selectionModel()->selectedIndexes().first();
    const QRect r = v->visualRect(cur.siblingAtColumn(0));
    const QRect area = v->viewport()->rect();
    const QPoint at = r.isValid() && area.intersects(r) ? QPoint(r.left() + r.height(), r.bottom()) : area.center();
    return v->viewport()->mapToGlobal(at);
}

void BrowserTab::setShowHidden(bool show)
{
    m_fs->setFilter(kBaseFilter | (show ? QDir::Hidden : QDir::Filters()));
}

void BrowserTab::setSearch(const QString &text)
{
    m_proxy->setSearch(text);
}

void BrowserTab::applyFolderPrefs()
{
    const QVariantMap m = folderStore().value(folderKey(m_path)).toMap();
    if (m.isEmpty())
        return; // no remembered view: keep the tab's current one
    m_applyingPrefs = true;
    const Mode md = Mode(qBound(0, m.value(QStringLiteral("mode"), int(m_mode)).toInt(), 2));
    if (md != m_mode) {
        m_mode = md;
        m_stack->setCurrentIndex(md);
    }
    if (m.contains(QStringLiteral("header")))
        m_list->header()->restoreState(m.value(QStringLiteral("header")).toByteArray());
    setFinderColumnOrder(m_list->header());
    if (m.contains(QStringLiteral("iconSize")))
        setIconSize(m.value(QStringLiteral("iconSize")).toInt());
    m_applyingPrefs = false;
    emit modeChanged();
}

void BrowserTab::saveFolderPrefs()
{
    if (m_applyingPrefs || m_path.isEmpty())
        return;
    folderStore().setValue(folderKey(m_path), QVariantMap{
        {QStringLiteral("mode"), int(m_mode)},
        {QStringLiteral("header"), m_list->header()->saveState()},
        {QStringLiteral("iconSize"), m_iconSize},
    });
}

void BrowserTab::setIconSize(int size, bool remember)
{
    m_iconSize = qBound(32, size, 256);
    m_gallery->setIconSize(QSize(m_iconSize, m_iconSize));
    const int lineH = m_gallery->fontMetrics().height() + 2;
    m_gallery->setGridSize(QSize(m_iconSize + 48, m_iconSize + 16 + 14 + 2 * lineH));
    if (remember)
        saveFolderPrefs();
}

int BrowserTab::itemCount() const
{
    return m_proxy->rowCount(m_proxy->indexForPath(m_path));
}

// List view, Finder-style: → expands a folder (⌥→ everything inside it), or steps into an
// already expanded one; ← collapses, or moves up to the enclosing folder's row.
bool BrowserTab::listRight(bool recursive)
{
    const QModelIndex cur = m_list->currentIndex().siblingAtColumn(0);
    if (!m_proxy->isDir(cur) || util::isPackage(QFileInfo(m_proxy->filePath(cur))))
        return true;
    if (recursive) {
        m_list->expandRecursively(cur);
        return true;
    }
    if (!m_list->isExpanded(cur)) {
        m_list->expand(cur);
        return true;
    }
    if (const QModelIndex child = m_proxy->index(0, 0, cur); child.isValid())
        selectIndexes({child});
    return true;
}

bool BrowserTab::listLeft(bool recursive)
{
    const QModelIndex cur = m_list->currentIndex().siblingAtColumn(0);
    if (!cur.isValid()) {
        emit leftEdgeReached();
        return true;
    }
    if (m_list->isExpanded(cur)) {
        if (recursive) {
            std::function<void(const QModelIndex &)> collapseAll = [&](const QModelIndex &i) {
                for (int r = 0; r < m_proxy->rowCount(i); ++r)
                    if (const QModelIndex c = m_proxy->index(r, 0, i); m_list->isExpanded(c))
                        collapseAll(c);
                m_list->collapse(i);
            };
            collapseAll(cur);
        } else {
            m_list->collapse(cur);
        }
        return true;
    }
    if (cur.parent() != m_list->rootIndex() && cur.parent().isValid())
        selectIndexes({cur.parent()});
    else
        emit leftEdgeReached();
    return true;
}

bool BrowserTab::eventFilter(QObject *obj, QEvent *ev)
{
    auto *widget = qobject_cast<QWidget *>(obj);
    if (!widget || !isAncestorOf(widget))
        return false;
    auto *v = qobject_cast<QAbstractItemView *>(obj);
    if (ev->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        if (v && QApplication::focusWidget() == v) {
            for (const QString &id : {QStringLiteral("사이드바로 이동"), QStringLiteral("파일뷰로 이동"), QStringLiteral("터미널로 이동"),
                                     QStringLiteral("열기"), QStringLiteral("이름 변경"), QStringLiteral("퀵 뷰어")})
                if (Shortcuts::instance()->matches(id, ke)) {
                    ke->ignore();
                    return true;
                }
        }
        // File-operation keys apply to the views; a rename editor keeps normal text editing.
        if (qobject_cast<QLineEdit *>(widget)) {
            for (const QString &id : {QStringLiteral("열기"), QStringLiteral("이름 변경"), QStringLiteral("퀵 뷰어")})
                if (Shortcuts::instance()->matches(id, ke)) {
                    ke->accept();
                    return true;
                }
        }
        return false;
    }
    if (ev->type() != QEvent::KeyPress || !v || QApplication::focusWidget() != v)
        return false;
    auto *ke = static_cast<QKeyEvent *>(ev);
    const auto matches = [ke](const char *id) { return Shortcuts::instance()->matches(QString::fromUtf8(id), ke); };
    if (ke->key() == Qt::Key_Down && !(ke->modifiers() & ~Qt::KeypadModifier)
        && !v->selectionModel()->hasSelection()) {
        const QModelIndex first = v->model()->index(0, 0, v->rootIndex());
        if (first.isValid()) {
            selectIndexes({first});
            return true;
        }
    }
    if (matches("폴더 펼치기") || matches("하위 폴더 모두 펼치기")) {
        if (m_mode == Columns) {
            const QModelIndex cur = v->currentIndex();
            if (m_proxy->isDir(cur) && !util::isPackage(QFileInfo(m_proxy->filePath(cur)))) {
                openIndex(cur, false);
                return true;
            }
        }
        if (m_mode == List)
            return listRight(matches("하위 폴더 모두 펼치기"));
    }
    if (m_mode == List && (matches("폴더 접기") || matches("하위 폴더 모두 접기")))
        return listLeft(matches("하위 폴더 모두 접기"));
    // Reassigning or clearing the fold keys must also disable Qt's built-in tree bindings.
    if ((m_mode == List || m_mode == Columns) && (ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right)
        && !(ke->modifiers() & ~Qt::KeypadModifier))
        return true;
    return false;
}
