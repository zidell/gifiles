#include "ItemDelegate.h"
#include "Settings.h"
#include "Shortcuts.h"
#include "App.h"
#include "FileProxy.h"
#include "Theme.h"
#include "Thumbnails.h"
#include "Util.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStyle>
#include <QTextLayout>
#include <QTimer>

namespace {
constexpr int kRowHeight = 24;
int defaultLineHeight() // the row height's reference: the app font's line, read once the app exists
{
    static const int h = qMax(1, QFontMetrics(QApplication::font()).height());
    return h;
}
constexpr int kTilePad = 8;

// Splits a name into at most two lines for gallery tiles; the second line is middle-elided.
QStringList twoLines(const QString &text, const QFont &font, int width)
{
    const QFontMetrics fm(font);
    if (fm.horizontalAdvance(text) <= width)
        return {text};
    QTextLayout layout(text, font);
    QTextOption to;
    to.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(to);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setLineWidth(width);
    const int n = line.textLength();
    layout.endLayout();
    return {text.left(n).trimmed(), fm.elidedText(text.mid(n), Qt::ElideMiddle, width)};
}
} // namespace

ItemDelegate::ItemDelegate(FileProxy *proxy, Kind kind, QObject *parent)
    : QStyledItemDelegate(parent), m_proxy(proxy), m_kind(kind)
{
}

QPixmap ItemDelegate::thumbnail(const QModelIndex &index) const
{
    if (m_kind != Gallery || index.column() != ColName)
        return {};
    const QModelIndex src = m_proxy->mapToSource(index);
    if (m_proxy->fs()->isDir(src))
        return {};
    const QString path = m_proxy->fs()->filePath(src);
    if (!Thumbnails::canThumbnail(path))
        return {};
    return Thumbnails::instance()->get(path, m_proxy->fs()->lastModified(src));
}

void ItemDelegate::initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    option->state &= ~QStyle::State_HasFocus; // no focus rectangle; selection shows the current item
    if (index.column() != ColName)
        return;
    if (Settings::instance()->flag(Settings::UppercaseNames))
        option->text = option->text.toUpper(); // shown only: the rename editor gets the real name
    // Platforms without an icon theme give no icon at all, which also collapses the gallery layout.
    if (option->icon.isNull()) {
        const QStyle *st = option->widget ? option->widget->style() : QApplication::style();
        option->icon = st->standardIcon(m_proxy->isDir(index) ? QStyle::SP_DirIcon : QStyle::SP_FileIcon);
    }
    option->features |= QStyleOptionViewItem::HasDecoration;
}

QSize ItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (m_kind == Gallery) {
        const int s = option.decorationSize.width();
        return QSize(s + 2 * kTilePad + 24, s + 2 * kTilePad + 8 + 2 * option.fontMetrics.height() + 10);
    }
    QSize sz = QStyledItemDelegate::sizeHint(option, index);
    // 24 px at the default size; grows and shrinks with the views' font (⌘+ / ⌘−).
    sz.setHeight(qMax(sz.height(), qMax(kRowHeight * option.fontMetrics.height() / defaultLineHeight(), option.fontMetrics.height() + 6)));
    return sz;
}

QRect ItemDelegate::tileRect(const QStyleOptionViewItem &opt) const
{
    const int s = opt.decorationSize.width() + 2 * kTilePad;
    return QRect(opt.rect.center().x() - s / 2, opt.rect.top() + 4, s, s);
}

void ItemDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // Cut items are dimmed until they're pasted somewhere.
    const qreal opacity = p->opacity();
    if (App::instance()->isCut(m_proxy->filePath(index.siblingAtColumn(0))))
        p->setOpacity(opacity * 0.45);
    paintItem(p, option, index);
    p->setOpacity(opacity);
}

bool ItemDelegate::isFolder(const QModelIndex &index) const
{
    return m_proxy->isDir(index) && !util::isPackage(QFileInfo(m_proxy->filePath(index.siblingAtColumn(0))));
}

QColor ItemDelegate::nameStyle(const QModelIndex &index, const QColor &plain, QFont *font) const
{
    if (font && Settings::instance()->flag(Settings::BoldNames))
        font->setBold(true);
    if (isFolder(index)) {
        if (font)
            font->setBold(true);
        const QColor c = Theme::folderColor();
        return c.isValid() ? c : plain;
    }
    const QColor c = Theme::fileColor(index.data().toString());
    return c.isValid() ? c : plain;
}

QColor ItemDelegate::selectionColor(const QModelIndex &index) const
{
    // Names keep their colors on one accent bar; or (file_colors.selection) the bar takes each item's color.
    if (!Settings::instance()->flag(Settings::FileColorSelection))
        return Theme::colors().nameSelection;
    const QModelIndex name = index.siblingAtColumn(ColName);
    const QColor c = isFolder(name) ? Theme::folderBaseColor() : Theme::fileBaseColor(name.data().toString());
    return Theme::selectionFill(c.isValid() ? c : Theme::plainSelectionColor());
}

void ItemDelegate::paintItem(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (m_kind == Gallery) {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        paintTile(p, opt, index);
        return;
    }
    // Names are tinted by kind and folders are bold in every view (nameStyle); in the list, date,
    // size and kind are dimmer than the name.
    QStyleOptionViewItem opt(option);
    QColor text = opt.palette.color(QPalette::Text);
    if (index.column() == ColName)
        text = nameStyle(index, text, &opt.font);
    else
        text.setAlphaF(0.5);
    // Selected in an active view: drawn as unselected text on the selection bar (the list's pill is
    // drawRow's; the column view's is painted here). Names keep their colors unless the bar is the
    // item's own color, where the text is black.
    const bool sel = option.state & QStyle::State_Selected;
    const bool active = option.state & QStyle::State_Active;
    const QColor own = sel && active ? selectionColor(index) : QColor();
    if (own.isValid()) {
        opt.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        if (Settings::instance()->flag(Settings::FileColorSelection)) {
            text = Theme::selectionTextColor();
            if (index.column() != ColName)
                text.setAlphaF(0.65);
        }
        if (m_kind == Column) {
            QPainterPath pill;
            pill.addRoundedRect(QRectF(option.rect), 7, 7);
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->fillPath(pill, own);
            p->restore();
        }
    }
    opt.palette.setColor(QPalette::Text, text);
    QStyledItemDelegate::paint(p, opt, index);
    if (m_kind == Column && index.column() == ColName && m_proxy->isDir(index) &&
        !util::isPackage(QFileInfo(m_proxy->filePath(index)))) {
        const QColor col = !own.isValid() ? Theme::colors().secondary
                           : Settings::instance()->flag(Settings::FileColorSelection) ? Theme::selectionTextColor()
                                                                                       : Theme::colors().text;
        const QRect r(option.rect.right() - 20, option.rect.center().y() - 7, 14, 14);
        Theme::icon(QStringLiteral("chevron-right-small"), col, 14).paint(p, r);
    }
}

void ItemDelegate::paintTile(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    const Theme::Colors &c = Theme::colors();
    const bool selected = opt.state & QStyle::State_Selected;
    const bool hover = opt.state & QStyle::State_MouseOver;
    const bool active = opt.state & QStyle::State_Active;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);

    const QRect tile = tileRect(opt);
    if (selected || hover) {
        QPainterPath bg;
        bg.addRoundedRect(QRectF(tile), 10, 10);
        p->fillPath(bg, selected ? c.selInactive : c.hover);
    }
    const int s = opt.decorationSize.width();
    const QRect iconRect(tile.center().x() - s / 2 + 1, tile.center().y() - s / 2 + 1, s, s);
    const QPixmap thumb = thumbnail(index);
    if (!thumb.isNull()) {
        // Photos: fit inside the icon square with rounded corners and a hairline border.
        QSizeF fitted = QSizeF(thumb.size() / thumb.devicePixelRatio()).scaled(QSizeF(iconRect.size()), Qt::KeepAspectRatio);
        QRectF tr(QPointF(0, 0), fitted);
        tr.moveCenter(QRectF(iconRect).center());
        QPainterPath clip;
        clip.addRoundedRect(tr, 5, 5);
        p->setClipPath(clip);
        p->drawPixmap(tr, thumb, QRectF(thumb.rect()));
        p->setClipping(false);
        p->setPen(QPen(c.separator, 1));
        p->drawPath(clip);
    } else {
        opt.icon.paint(p, iconRect, Qt::AlignCenter, QIcon::Normal);
    }

    // Name: up to two centered lines; selected names sit on an accent pill like Finder.
    QFont font = opt.font;
    const QColor nameColor = nameStyle(index, c.text, &font);
    const QFontMetrics fm(font);
    const QColor own = selected && active ? selectionColor(index) : QColor();
    const int w = opt.rect.width() - 8;
    const QStringList lines = twoLines(opt.text, font, w);
    p->setFont(font);
    int y = tile.bottom() + 6;
    for (const QString &line : lines) {
        const int lw = qMin(w, fm.horizontalAdvance(line)) + 10;
        const QRect lr(opt.rect.center().x() - lw / 2, y, lw, fm.height() + 2);
        if (selected) {
            QPainterPath pill;
            pill.addRoundedRect(QRectF(lr), 4, 4);
            p->fillPath(pill, !active ? c.selInactive : own.isValid() ? own : c.selection);
        }
        p->setPen(own.isValid() && Settings::instance()->flag(Settings::FileColorSelection) ? Theme::selectionTextColor() : nameColor);
        p->drawText(lr, Qt::AlignCenter, line);
        y += fm.height() + 2;
    }
    p->restore();
}

QWidget *ItemDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QWidget *editor = QStyledItemDelegate::createEditor(parent, option, index);
    editor->setObjectName(QStringLiteral("renameEdit")); // no padding: see updateEditorGeometry
    if (isFolder(index) || Settings::instance()->flag(Settings::BoldNames)) { // drawn bold: the text mustn't move when editing starts
        QFont f = editor->font();
        f.setBold(true);
        editor->setFont(f);
    }
    // If it closes without Return or Esc (its row went away), keep what was typed for
    // takeLostEdit. Watched by an object of our own: the view removes the delegate's event filter
    // before it hides a closing editor.
    if (auto *le = qobject_cast<QLineEdit *>(editor)) {
        struct Watch : QObject {
            std::function<void()> onHide;
            using QObject::QObject;
            bool eventFilter(QObject *, QEvent *ev) override
            {
                if (ev->type() == QEvent::Hide && onHide)
                    onHide();
                return false;
            }
        };
        auto *w = new Watch(le);
        const QString path = m_proxy->filePath(index);
        w->onHide = [this, le, path] {
            if (le->property("done").toBool() || le->text().isEmpty())
                return;
            m_lost = LostEdit{path, le->text(), {}};
            m_lost->at.start();
        };
        le->installEventFilter(w);
    }
    return editor;
}

void ItemDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (m_kind != Gallery) {
        // The editor's text starts exactly where the row's name is drawn, so nothing moves when
        // editing starts or Return commits it.
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QWidget *w = opt.widget;
        const QStyle *st = w ? w->style() : QApplication::style();
        QRect r = st->subElementRect(QStyle::SE_ItemViewItemText, &opt, w);
        int drawn = st->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, w) + 1; // QCommonStyle's text margin
        if (m_kind == Column)
            drawn -= 1; // the column lists' QSS item padding draws 1 px further left (measured on macOS)
        int inner = 2; // QLineEdit's own horizontal margin
        if (auto *le = qobject_cast<QLineEdit *>(editor)) {
            le->ensurePolished();
            inner += le->contentsRect().left() + le->textMargins().left();
        }
        r.adjust(drawn - inner, 0, 0, 0);
        editor->setGeometry(r);
        return;
    }
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QRect tile = tileRect(opt);
    // The name is drawn at tile.bottom() + 6 in a line fh + 2 high; the editor (border 1, no padding)
    // centers its text at the same height.
    editor->setGeometry(opt.rect.left() + 2, tile.bottom() + 3, opt.rect.width() - 4, opt.fontMetrics.height() + 8);
}

void ItemDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
    QStyledItemDelegate::setEditorData(editor, index);
    auto *le = qobject_cast<QLineEdit *>(editor);
    if (!le)
        return;
    if (m_kind == Gallery)
        le->setAlignment(Qt::AlignCenter);
    QString base, ext;
    util::splitExt(le->text(), &base, &ext);
    const int len = m_proxy->isDir(index) ? le->text().size() : base.size();
    // The view selects all text after this call; select the base name afterwards.
    QPointer<QLineEdit> guard(le);
    QTimer::singleShot(0, le, [guard, len] {
        if (guard)
            guard->setSelection(0, len);
    });
}

bool ItemDelegate::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::KeyPress || ev->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(ev);
        const bool commit = Shortcuts::instance()->matches(QStringLiteral("이름 변경 확정"), key);
        const bool cancel = Shortcuts::instance()->matches(QStringLiteral("이름 변경 취소"), key);
        if (commit || cancel) {
            if (ev->type() == QEvent::ShortcutOverride) {
                ev->accept();
                return true;
            }
            auto *editor = qobject_cast<QWidget *>(obj);
            obj->setProperty("done", true);
            if (commit)
                emit commitData(editor);
            emit closeEditor(editor, cancel ? QAbstractItemDelegate::RevertModelCache : QAbstractItemDelegate::NoHint);
            return true;
        }
        // Disable Qt's built-in accept/cancel when those keys have been reassigned.
        if (ev->type() == QEvent::KeyPress && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Escape))
            return true;
    }
    return QStyledItemDelegate::eventFilter(obj, ev);
}

std::optional<QString> ItemDelegate::takeLostEdit(const QString &path) const
{
    if (!m_lost || m_lost->path != path || m_lost->at.elapsed() > 2000)
        return std::nullopt;
    return std::exchange(m_lost, std::nullopt)->text;
}

void ItemDelegate::setModelData(QWidget *editor, QAbstractItemModel *, const QModelIndex &index) const
{
    auto *le = qobject_cast<QLineEdit *>(editor);
    if (!le)
        return;
    le->setProperty("done", true);
    const QString path = m_proxy->filePath(index);
    const QString name = le->text();
    if (name != index.data(Qt::DisplayRole).toString() && name != QFileInfo(path).fileName())
        emit renameRequested(path, name);
}
