#include "RecentFolders.h"
#include "Settings.h"
#include "Shortcuts.h"
#include "Sidebar.h"
#include "Theme.h"
#include "Util.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileIconProvider>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>

namespace {
constexpr int PathRole = Qt::UserRole + 1;
constexpr int ActivePathRole = Qt::UserRole + 2;
constexpr int SectionRole = Qt::UserRole + 3; // a section title's id: "favorites", "recent", "locations"
const QString kFoldedKey = QStringLiteral("sidebar/folded"); // the folded sections (state, QSettings)

} // namespace

QStringList Sidebar::defaultFavorites()
{
    QStringList out;
    auto add = [&](QStandardPaths::StandardLocation loc) {
        const QString p = QStandardPaths::writableLocation(loc);
        if (!p.isEmpty() && QDir(p).exists() && !out.contains(p))
            out << p;
    };
    add(QStandardPaths::HomeLocation);
#ifdef Q_OS_MACOS
    if (QDir(QStringLiteral("/Applications")).exists())
        out << QStringLiteral("/Applications");
#endif
    add(QStandardPaths::DesktopLocation);
    add(QStandardPaths::DocumentsLocation);
    add(QStandardPaths::DownloadLocation);
    add(QStandardPaths::PicturesLocation);
    add(QStandardPaths::MusicLocation);
    add(QStandardPaths::MoviesLocation);
    return out;
}

namespace {

// Line glyph and Korean label for well-known folders (Finder-style tinted sidebar symbols).
QPair<QString, QString> placeLook(const QString &path)
{
    const QString p = QDir::cleanPath(path);
    const struct {
        QStandardPaths::StandardLocation loc;
        const char *glyph;
        const char *label;
    } known[] = {
        {QStandardPaths::DesktopLocation, "desktop", QT_TRANSLATE_NOOP("Gifiles", "데스크탑")},
        {QStandardPaths::DocumentsLocation, "documents", QT_TRANSLATE_NOOP("Gifiles", "문서")},
        {QStandardPaths::DownloadLocation, "downloads", QT_TRANSLATE_NOOP("Gifiles", "다운로드")},
        {QStandardPaths::PicturesLocation, "pictures", QT_TRANSLATE_NOOP("Gifiles", "사진")},
        {QStandardPaths::MusicLocation, "music", QT_TRANSLATE_NOOP("Gifiles", "음악")},
        {QStandardPaths::MoviesLocation, "movies", QT_TRANSLATE_NOOP("Gifiles", "동영상")},
        {QStandardPaths::HomeLocation, "home", nullptr},
    };
    for (const auto &k : known)
        if (QDir::cleanPath(QStandardPaths::writableLocation(k.loc)) == p)
            return {QString::fromLatin1(k.glyph), k.label ? Gifiles::tr(k.label) : util::displayName(p)};
    if (p == QLatin1String("/Applications"))
        return {QStringLiteral("apps"), Gifiles::tr("응용 프로그램")};
    const QFileInfo fi(p);
    if (!fi.isDir() || util::isPackage(fi))
        return {QStringLiteral("documents"), util::displayName(p)};
    return {QStringLiteral("folder"), util::displayName(p)};
}

// Section titles are small grey labels; places are rounded rows with a tinted glyph.
class SidebarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &idx) const override
    {
        const bool title = !idx.parent().isValid();
        return QSize(o.rect.width(), title ? (idx.row() == 0 ? 26 : 34) : 28);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &idx) const override
    {
        const Theme::Colors &c = Theme::colors();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        QRect r = o.rect;
        r.setLeft(4); // ignore tree indentation: rows span the whole sidebar
        if (!idx.parent().isValid()) {
            QFont f = o.font;
            f.setPointSizeF(f.pointSizeF() * 0.85);
            f.setWeight(QFont::DemiBold);
            p->setFont(f);
            p->setPen(c.secondary);
            const QRect text = r.adjusted(8, 0, -4, -4);
            p->drawText(text, Qt::AlignLeft | Qt::AlignBottom, idx.data().toString());
            // Folds with one click: ⌄ open, › folded, at the row's right end as in Finder.
            const auto *tree = qobject_cast<const QTreeView *>(parent());
            const bool open = tree && tree->isExpanded(idx);
            const int h = QFontMetrics(f).height();
            const QRect chev(r.right() - 8 - 16, text.bottom() - h + (h - 16) / 2 + 1, 16, 16);
            p->setOpacity(0.5); // quieter than the title
            Theme::icon(open ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"), c.secondary, 16)
                .paint(p, chev);
            p->restore();
            return;
        }
        const bool selected = idx.data(ActivePathRole).toBool();
        const auto *sidebar = qobject_cast<const Sidebar *>(parent());
        const bool focused = sidebar && sidebar->hasFocus() && sidebar->currentIndex() == idx;
        const bool hover = o.state & QStyle::State_MouseOver;
        if (selected || focused || hover) {
            QPainterPath path;
            path.addRoundedRect(QRectF(r).adjusted(0, 1, 0, -1), 7, 7);
            p->fillPath(path, selected ? c.selection : c.hover);
        }
        const QIcon icon = idx.data(Qt::DecorationRole).value<QIcon>();
        const QRect ir(r.left() + 8, r.center().y() - 9, 18, 18);
        icon.paint(p, ir, Qt::AlignCenter, selected ? QIcon::Selected : QIcon::Normal); // white like the label
        p->setPen(selected ? c.selText : c.text);
        const QRect tr = r.adjusted(34, 0, -6, 0);
        p->drawText(tr, Qt::AlignVCenter | Qt::AlignLeft,
                    o.fontMetrics.elidedText(idx.data().toString(), Qt::ElideMiddle, tr.width()));
        p->restore();
    }
};
} // namespace

Sidebar::Sidebar(QWidget *parent) : QTreeWidget(parent)
{
    setObjectName(QStringLiteral("sidebar"));
    setItemDelegate(new SidebarDelegate(this));
    setMouseTracking(true);
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(0);
    setMinimumWidth(170);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setFrameShape(QFrame::NoFrame);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DropOnly);
    setIconSize(QSize(18, 18));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    rebuild();
    connect(this, &QTreeWidget::itemSelectionChanged, this, [this] { setCurrentPath(m_current); });
    connect(Theme::instance(), &Theme::changed, this, &Sidebar::rebuild);
    connect(this, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        const QString p = it->data(0, PathRole).toString();
        if (!p.isEmpty())
            emit placeActivated(p);
    });
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
        if (!it->parent()) // a section title: one click folds or unfolds it
            return it->setExpanded(!it->isExpanded());
        const QString p = it->data(0, PathRole).toString();
        if (!p.isEmpty())
            emit placeActivated(p);
    });
    // A double click is two clicks (fold, unfold): its second press comes as the double click.
    setExpandsOnDoubleClick(false);
    connect(this, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem *it) {
        if (!it->parent())
            it->setExpanded(!it->isExpanded());
    });
    // However it was folded (click, Return, ← / →), the next start shows it the same.
    auto remember = [this](QTreeWidgetItem *it) {
        if (it->parent())
            return;
        QStringList folded;
        for (QTreeWidgetItem *s : sections())
            if (!s->isExpanded())
                folded << s->data(0, SectionRole).toString();
        QSettings().setValue(kFoldedKey, folded);
        viewport()->update(); // the chevron
    };
    connect(this, &QTreeWidget::itemExpanded, this, remember);
    connect(this, &QTreeWidget::itemCollapsed, this, remember);
    // Queued: a file operation's record can arrive while the sidebar is in the middle of something.
    connect(RecentFolders::instance(), &RecentFolders::changed, this, &Sidebar::rebuild, Qt::QueuedConnection);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Sidebar::refreshVolumes);
    timer->start(4000);
}

QStringList Sidebar::favorites()
{
    return Settings::instance()->value(Settings::Favorites).toStringList(); // default: defaultFavorites()
}

void Sidebar::setFavorites(const QStringList &paths)
{
    Settings::instance()->setValue(Settings::Favorites, paths);
}

QTreeWidgetItem *Sidebar::addPlace(QTreeWidgetItem *section, const QString &path, const QString &label)
{
    auto *it = new QTreeWidgetItem(section);
    // Drawn glyphs rather than the system's folder icons: asking macOS for a folder's own icon reads
    // inside it, which triggers the Desktop/Documents/Downloads permission prompts at launch.
    const auto look = placeLook(path);
    const bool volume = section == m_locations;
    it->setText(0, !label.isEmpty() ? label : look.second);
    it->setIcon(0, Theme::icon(volume ? QStringLiteral("drive") : look.first,
                               volume ? Theme::colors().secondary : Theme::colors().accent));
    it->setData(0, PathRole, path);
    it->setToolTip(0, QDir::toNativeSeparators(path));
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
    return it;
}

void Sidebar::rebuild()
{
    QSignalBlocker block(this);
    const QString focusedPath = currentItem() ? currentItem()->data(0, PathRole).toString() : QString();
    clear();
    m_favorites = m_recent = m_locations = nullptr;
    auto section = [this](const QString &title, const char *id) {
        auto *s = new QTreeWidgetItem(this);
        s->setText(0, title);
        s->setData(0, SectionRole, QString::fromLatin1(id));
        s->setToolTip(0, Gifiles::tr("클릭하면 접거나 펼칩니다"));
        s->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
        return s;
    };
    m_favorites = section(Gifiles::tr("즐겨찾기"), "favorites");
    for (const QString &p : favorites())
        if (QFileInfo::exists(p))
            addPlace(m_favorites, p);
    // Shown while sidebar.recent_folders > 0, the title alone until the first folder comes in.
    if (Settings::instance()->value(Settings::RecentFoldersCount).toInt() > 0) {
        m_recent = section(Gifiles::tr("최근 폴더"), "recent");
        for (const QString &p : RecentFolders::instance()->folders())
            addPlace(m_recent, p);
    }
    m_locations = section(Gifiles::tr("위치"), "locations");
    m_volumeRoots.clear();
    for (const QStorageInfo &si : QStorageInfo::mountedVolumes()) {
        if (!util::isUserVolume(si))
            continue;
        QString label = si.displayName();
        if (label.isEmpty() || label == si.rootPath())
            label = si.rootPath() == QLatin1String("/") ? Gifiles::tr("컴퓨터") : util::displayName(si.rootPath());
        addPlace(m_locations, si.rootPath(), label);
        m_volumeRoots << si.rootPath();
    }
    const QStringList folded = QSettings().value(kFoldedKey).toStringList();
    for (QTreeWidgetItem *section : sections())
        section->setExpanded(!folded.contains(section->data(0, SectionRole).toString()));
    for (QTreeWidgetItem *section : sections())
        for (int i = 0; i < section->childCount(); ++i)
            if (!focusedPath.isEmpty() && section->child(i)->data(0, PathRole).toString() == focusedPath)
                setCurrentItem(section->child(i), 0, QItemSelectionModel::NoUpdate);
    setCurrentPath(m_current);
}

QList<QTreeWidgetItem *> Sidebar::sections() const
{
    QList<QTreeWidgetItem *> out;
    for (QTreeWidgetItem *s : {m_favorites, m_recent, m_locations})
        if (s)
            out << s;
    return out;
}

void Sidebar::refreshVolumes()
{
    QStringList roots;
    for (const QStorageInfo &si : QStorageInfo::mountedVolumes())
        if (util::isUserVolume(si))
            roots << si.rootPath();
    if (roots != m_volumeRoots)
        rebuild();
}

void Sidebar::setCurrentPath(const QString &path)
{
    m_current = path;
    const QString clean = QDir::cleanPath(path);
    QSignalBlocker block(this);
    QSignalBlocker selectionBlock(selectionModel());
    clearSelection();
    bool matched = false;
    for (QTreeWidgetItem *sec : sections())
        for (int i = 0; i < sec->childCount(); ++i) {
            auto *item = sec->child(i);
            const bool active = !matched && QDir::cleanPath(item->data(0, PathRole).toString()) == clean;
            item->setData(0, ActivePathRole, active);
            item->setSelected(active);
            matched = matched || active;
        }
    viewport()->update();
}

void Sidebar::keyPressEvent(QKeyEvent *e)
{
    if (Shortcuts::instance()->matches(QStringLiteral("사이드바 항목 열기"), e)) {
        if (QTreeWidgetItem *it = currentItem(); it && !it->parent())
            it->setExpanded(!it->isExpanded()); // a section title: Return folds / unfolds it
        else if (it)
            emit itemActivated(it, 0);
        return;
    }
    if (Shortcuts::instance()->matches(QStringLiteral("사이드바에서 파일뷰로 이동"), e)) {
        emit rightPressed();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        return; // Qt must not reactivate the old key after it is reassigned.
    QTreeWidget::keyPressEvent(e);
}

namespace {
const QString kFavoriteMime = QStringLiteral("application/x-gifiles-favorite");
}

QStringList Sidebar::shownFavorites() const
{
    QStringList out;
    for (int i = 0; m_favorites && i < m_favorites->childCount(); ++i)
        out << m_favorites->child(i)->data(0, PathRole).toString();
    return out;
}

void Sidebar::insertFavorites(const QStringList &paths, int index)
{
    // Work on what is shown (entries whose path no longer exists are hidden but kept at the end).
    QStringList shown = shownFavorites();
    QStringList hidden;
    for (const QString &f : favorites())
        if (!shown.contains(f))
            hidden << f;
    index = qBound(0, index, int(shown.size()));
    for (const QString &raw : paths) {
        const QString p = QDir::cleanPath(raw);
        if (const int at = shown.indexOf(p); at >= 0) {
            shown.removeAt(at);
            if (at < index)
                --index;
        }
        shown.insert(index++, p);
    }
    setFavorites(shown + hidden);
    rebuild();
}

Sidebar::DropTarget Sidebar::dropTargetAt(const QPoint &pos, bool internal) const
{
    DropTarget t;
    auto insertAt = [this, &t](int index) {
        const int n = m_favorites->childCount();
        t.kind = DropTarget::Insert;
        t.index = qBound(0, index, n);
        if (n == 0)
            t.lineY = visualItemRect(m_favorites).bottom() + 1;
        else if (t.index < n)
            t.lineY = visualItemRect(m_favorites->child(t.index)).top();
        else
            t.lineY = visualItemRect(m_favorites->child(n - 1)).bottom() + 1;
    };
    QTreeWidgetItem *it = itemAt(pos);
    if (!it || it == m_locations || it == m_recent) { // empty space or a later title: append
        insertAt(m_favorites->childCount());
        return t;
    }
    if (it == m_favorites) {
        insertAt(0);
        return t;
    }
    const QString path = it->data(0, PathRole).toString();
    if (it->parent() == m_locations || (m_recent && it->parent() == m_recent)) {
        if (!internal) {
            t.kind = DropTarget::Into;
            t.intoPath = path;
        }
        return t;
    }
    // A favorite row: the top/bottom quarter (or anywhere, when reordering) means "insert here";
    // the middle of a folder row means "drop into that folder".
    const QRect r = visualItemRect(it);
    const int row = m_favorites->indexOfChild(it);
    const int y = pos.y();
    const QFileInfo fi(path);
    const bool folder = fi.isDir() && !util::isPackage(fi);
    if (internal || !folder || y < r.top() + r.height() / 4 || y > r.bottom() - r.height() / 4) {
        insertAt(y < r.center().y() ? row : row + 1);
        return t;
    }
    t.kind = DropTarget::Into;
    t.intoPath = path;
    return t;
}

void Sidebar::clearDropFeedback()
{
    m_dropLineY = -1;
    viewport()->update();
    setCurrentPath(m_current);
}

void Sidebar::mousePressEvent(QMouseEvent *e)
{
    m_pressPos = e->position().toPoint();
    QTreeWidgetItem *it = itemAt(m_pressPos);
    m_pressFavorite = it && it->parent() == m_favorites ? it->data(0, PathRole).toString() : QString();
    QTreeWidget::mousePressEvent(e);
}

void Sidebar::mouseMoveEvent(QMouseEvent *e)
{
    // Dragging a favorite reorders it.
    if ((e->buttons() & Qt::LeftButton) && !m_pressFavorite.isEmpty() &&
        (e->position().toPoint() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        auto *md = new QMimeData;
        md->setData(kFavoriteMime, m_pressFavorite.toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(md);
        if (QTreeWidgetItem *it = itemAt(m_pressPos))
            drag->setPixmap(viewport()->grab(visualItemRect(it)));
        m_pressFavorite.clear();
        drag->exec(Qt::MoveAction);
        clearDropFeedback();
        return;
    }
    QTreeWidget::mouseMoveEvent(e);
}

void Sidebar::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasFormat(kFavoriteMime))
        e->acceptProposedAction();
    else
        e->ignore();
}

void Sidebar::dragLeaveEvent(QDragLeaveEvent *e)
{
    clearDropFeedback();
    QTreeWidget::dragLeaveEvent(e);
}

void Sidebar::dragMoveEvent(QDragMoveEvent *e)
{
    const bool internal = e->mimeData()->hasFormat(kFavoriteMime);
    const DropTarget t = dropTargetAt(e->position().toPoint(), internal);
    m_dropLineY = t.kind == DropTarget::Insert ? t.lineY : -1;
    viewport()->update();
    switch (t.kind) {
    case DropTarget::None:
        setCurrentPath(m_current);
        e->ignore();
        return;
    case DropTarget::Insert:
        setCurrentPath(m_current);
        e->setDropAction(internal ? Qt::MoveAction : Qt::LinkAction);
        e->accept();
        return;
    case DropTarget::Into:
        if (QTreeWidgetItem *it = itemAt(e->position().toPoint())) {
            QSignalBlocker block(this);
            clearSelection();
            it->setSelected(true);
        }
        e->acceptProposedAction();
        return;
    }
}

void Sidebar::dropEvent(QDropEvent *e)
{
    const bool internal = e->mimeData()->hasFormat(kFavoriteMime);
    const DropTarget t = dropTargetAt(e->position().toPoint(), internal);
    const QList<QUrl> urls = e->mimeData()->urls();
    const QByteArray favorite = e->mimeData()->data(kFavoriteMime);
    clearDropFeedback();
    if (t.kind == DropTarget::Insert) {
        QStringList paths;
        if (internal)
            paths << QString::fromUtf8(favorite);
        for (const QUrl &u : urls)
            if (u.isLocalFile())
                paths << u.toLocalFile();
        // Rebuilding destroys items; do it after the drop event has finished.
        QTimer::singleShot(0, this, [this, paths, index = t.index] { insertFavorites(paths, index); });
        e->setDropAction(internal ? Qt::MoveAction : Qt::LinkAction);
        e->accept();
        return;
    }
    if (t.kind == DropTarget::Into) {
        // Handled asynchronously by the window (conflict prompts must not run inside the drag loop).
        const Qt::DropAction action = e->proposedAction();
        const Qt::KeyboardModifiers mods = e->modifiers();
        QTimer::singleShot(0, this, [this, urls, target = t.intoPath, action, mods] { emit dropRequested(urls, target, action, mods); });
        e->accept();
    }
}

void Sidebar::paintEvent(QPaintEvent *e)
{
    QTreeWidget::paintEvent(e);
    if (m_dropLineY < 0)
        return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = Theme::colors().accent;
    p.setPen(QPen(c, 2));
    p.drawLine(QPointF(18, m_dropLineY), QPointF(viewport()->width() - 8, m_dropLineY));
    p.setBrush(Theme::colors().sidebarBg);
    p.drawEllipse(QPointF(14, m_dropLineY), 3.5, 3.5);
}

void Sidebar::contextMenuEvent(QContextMenuEvent *e)
{
    QTreeWidgetItem *it = itemAt(e->pos());
    const bool favorite = it && it->parent() == m_favorites;
    const bool recent = it && m_recent && it->parent() == m_recent;
    if (!favorite && !recent)
        return;
    const QString path = it->data(0, PathRole).toString();
    QMenu menu(this);
    menu.addAction(Gifiles::tr("새로운 탭에서 열기"), this, [this, path] { emit openInNewTab(path); });
    if (favorite) {
        menu.addAction(Gifiles::tr("사이드바에서 제거"), this, [this, path] {
            QStringList favs = favorites();
            favs.removeAll(path);
            setFavorites(favs);
            rebuild();
        });
    } else {
        if (!shownFavorites().contains(path))
            menu.addAction(Gifiles::tr("즐겨찾기에 추가"), this, [this, path] { insertFavorites({path}, int(shownFavorites().size())); });
        menu.addAction(Gifiles::tr("최근 폴더에서 제거"), this, [path] { RecentFolders::instance()->remove(path); });
        menu.addAction(Gifiles::tr("최근 폴더 모두 지우기"), this, [] { RecentFolders::instance()->clear(); });
    }
    menu.exec(e->globalPos());
}
