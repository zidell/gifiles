#include "Theme.h"
#include "Settings.h"
#include "Util.h"
#include <QTimer>

#include <QApplication>
#include <QMimeDatabase>
#include <algorithm>
#include <array>
#include <cmath>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QHash>
#include <QAbstractItemView>
#include <QPainter>
#include <QProxyStyle>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleOptionMenuItem>
#include <QStyleHints>
#include <QSvgRenderer>

namespace {

// 24×24 line glyphs (stroke = currentColor), drawn for this app.
const QHash<QString, QString> &glyphs()
{
    static const QHash<QString, QString> g = {
        {"chevron-left", "<path d='M15 18l-6-6 6-6'/>"},
        {"keyboard", "<rect x='2.5' y='6' width='19' height='12' rx='2.5'/><path d='M6 10h.01M9.5 10h.01M13 10h.01M16.5 10h.01M7.5 14h9'/>"},
        {"chevron-right", "<path d='M9 18l6-6-6-6'/>"},
        {"chevron-right-small", "<path d='M10 16l4-4-4-4'/>"},
        {"chevron-down-small", "<path d='M8 10l4 4 4-4'/>"},
        {"chevron-up-small", "<path d='M8 14l4-4 4 4'/>"},
        {"sparkle", "<path d='M11 3.5l1.6 4.4 4.4 1.6-4.4 1.6L11 15.5l-1.6-4.4L5 9.5l4.4-1.6z'/><path d='M18.5 14.5l.8 2.2 2.2.8-2.2.8-.8 2.2-.8-2.2-2.2-.8 2.2-.8z'/>"},
        {"list", "<path d='M9 6h11M9 12h11M9 18h11'/><path d='M4.5 6h.01M4.5 12h.01M4.5 18h.01' stroke-width='2.6'/>"},
        {"grid", "<rect x='4' y='4' width='6.5' height='6.5' rx='1.5'/><rect x='13.5' y='4' width='6.5' height='6.5' rx='1.5'/>"
                 "<rect x='4' y='13.5' width='6.5' height='6.5' rx='1.5'/><rect x='13.5' y='13.5' width='6.5' height='6.5' rx='1.5'/>"},
        {"columns", "<rect x='3.5' y='4.5' width='17' height='15' rx='2.5'/><path d='M9.2 4.5v15M14.8 4.5v15'/>"},
        {"search", "<circle cx='11' cy='11' r='6.5'/><path d='M20 20l-4.2-4.2'/>"},
        {"plus", "<path d='M12 5v14M5 12h14'/>"},
        {"close-small", "<path d='M8.5 8.5l7 7M15.5 8.5l-7 7'/>"},
        {"sidebar", "<rect x='3.5' y='4.5' width='17' height='15' rx='2.5'/><path d='M9.5 4.5v15'/>"},
        {"preview", "<rect x='3.5' y='4.5' width='17' height='15' rx='2.5'/><path d='M14.5 4.5v15'/>"},
        {"terminal", "<rect x='3' y='4.5' width='18' height='15' rx='2.5'/><path d='M7.5 9.5l3 2.5-3 2.5M12.5 15h4'/>"},
        {"open", "<path d='M14 4.5h5.5V10M19.5 4.5L11 13'/><path d='M18 14v4a2 2 0 0 1-2 2H6.5a2 2 0 0 1-2-2V8.5a2 2 0 0 1 2-2H10'/>"},
        {"tab", "<rect x='3.5' y='5.5' width='17' height='13' rx='2.5'/><path d='M3.5 9.5h17M9.5 5.5v4'/>"},
        {"eye", "<path d='M2.5 12s3.5-6.5 9.5-6.5 9.5 6.5 9.5 6.5-3.5 6.5-9.5 6.5S2.5 12 2.5 12z'/><circle cx='12' cy='12' r='2.8'/>"},
        {"info", "<circle cx='12' cy='12' r='8.5'/><path d='M12 11v5.5M12 7.8h.01'/>"},
        {"pencil", "<path d='M15.5 4.5l4 4L9 19H5v-4z'/><path d='M13 7l4 4'/>"},
        {"copy", "<rect x='8.5' y='8.5' width='11' height='11' rx='2'/><path d='M15.5 8.5V6.5a2 2 0 0 0-2-2h-7a2 2 0 0 0-2 2v7a2 2 0 0 0 2 2h2'/>"},
        {"folder-plus", "<path d='M3.5 7.5a2 2 0 0 1 2-2h4l2 2h7a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z'/><path d='M12 10.5v5M9.5 13h5'/>"},
        {"link", "<path d='M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1'/><path d='M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1'/>"},
        {"scissors", "<circle cx='6.5' cy='7' r='2.5'/><circle cx='6.5' cy='17' r='2.5'/><path d='M8.6 8.4L19 17M8.6 15.6L19 7'/>"},
        {"clipboard", "<rect x='6' y='5' width='12' height='15.5' rx='2'/><path d='M9.5 5V3.8h5V5'/>"},
        {"trash", "<path d='M4.5 7h15M9.5 7V4.5h5V7M6.5 7l1 12.5h9l1-12.5'/>"},
        {"window", "<rect x='3.5' y='4.5' width='17' height='15' rx='2.5'/><path d='M3.5 8.5h17'/><path d='M6.3 6.5h.01M8.5 6.5h.01' stroke-width='2.2'/>"},
        {"move", "<path d='M5 12h13M14 7.5l4.5 4.5-4.5 4.5'/>"},
        {"more", "<path d='M6 12h.01M12 12h.01M18 12h.01' stroke-width='3'/>"},
        {"palette", "<path d='M12 3.5a8.5 8.5 0 1 0 0 17c1.1 0 1.7-.8 1.7-1.6 0-1.1-.9-1.5-.9-2.5 0-.9.7-1.5 1.6-1.5h2.2a3.9 3.9 0 0 0 3.9-3.9C20.5 7 16.7 3.5 12 3.5z'/>"
                    "<circle cx='7.5' cy='11.5' r='1'/><circle cx='9.5' cy='7.5' r='1'/><circle cx='14.5' cy='7.5' r='1'/>"},
        {"home", "<path d='M4 10.5L12 4l8 6.5V19a1 1 0 0 1-1 1h-4.5v-5.5h-5V20H5a1 1 0 0 1-1-1z'/>"},
        {"apps", "<path d='M12 3.5l2.4 5.2 5.6.6-4.2 3.8 1.2 5.5L12 15.8l-5 2.8 1.2-5.5L4 9.3l5.6-.6z'/>"},
        {"desktop", "<rect x='3' y='4.5' width='18' height='12' rx='2'/><path d='M9 20h6M12 16.5V20'/>"},
        {"documents", "<path d='M14 3.5H7.5a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2h9a2 2 0 0 0 2-2V8z'/><path d='M14 3.5V8h4.5M9 13h6M9 16.5h4'/>"},
        {"downloads", "<circle cx='12' cy='12' r='8.5'/><path d='M12 8v7.5M8.5 12.5L12 16l3.5-3.5'/>"},
        {"pictures", "<rect x='3.5' y='4.5' width='17' height='15' rx='2.5'/><circle cx='9' cy='10' r='1.6'/><path d='M20.5 16l-4.5-4.5-8 8'/>"},
        {"music", "<path d='M9 17.5V6l10-2v11.5'/><circle cx='6.5' cy='17.5' r='2.5'/><circle cx='16.5' cy='15.5' r='2.5'/>"},
        {"movies", "<rect x='3.5' y='5' width='17' height='14' rx='2.5'/><path d='M10 9.2v5.6l4.6-2.8z'/>"},
        {"folder", "<path d='M3.5 7.5a2 2 0 0 1 2-2h4l2 2h7a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z'/>"},
        {"drive", "<rect x='3' y='12.5' width='18' height='7' rx='2'/><path d='M3.5 14l2.8-7.5h11.4L20.5 14'/><path d='M7 16h.01' stroke-width='2.4'/>"},
        {"shield", "<path d='M12 3.5l7 2.8v5.4c0 4.4-3 7.6-7 8.8-4-1.2-7-4.4-7-8.8V6.3z'/><path d='M9 12l2.2 2.2L15.5 10'/>"},
    };
    return g;
}

QPixmap renderGlyph(const QString &body, const QColor &color, int size, qreal dpr)
{
    // SVG has no #AARRGGBB; alpha goes in stroke-opacity.
    const QString svg = QStringLiteral(
                            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%1' "
                            "stroke-opacity='%3' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'>%2</svg>")
                            .arg(color.name(QColor::HexRgb), body)
                            .arg(color.alphaF(), 0, 'f', 3);
    QSvgRenderer r(svg.toUtf8());
    QPixmap pm(QSize(size, size) * dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    r.render(&p);
    p.end();
    pm.setDevicePixelRatio(dpr);
    return pm;
}

// Stylesheets can only reference image files, so tinted glyphs used by QSS are written to the cache.
QString glyphFile(const QString &name, const QColor &color)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/theme");
    QDir().mkpath(dir);
    const QString path = QStringLiteral("%1/%2-%3.svg").arg(dir, name, color.name(QColor::HexRgb).mid(1));
    if (!QFile::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' "
                                   "fill='none' stroke='%1' stroke-width='2' stroke-linecap='round' "
                                   "stroke-linejoin='round'>%2</svg>")
                        .arg(color.name(QColor::HexRgb), glyphs().value(name))
                        .toUtf8());
    }
    return path;
}

// Toggle switch artwork for QCheckBox#switch (QSS can only use image files).
QString switchFile(bool on, const QColor &track, const QColor &knob)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/theme");
    QDir().mkpath(dir);
    const QString path = QStringLiteral("%1/switch-%2-%3-%4.svg").arg(dir, on ? QStringLiteral("on") : QStringLiteral("off"),
                                                                     track.name(QColor::HexArgb).mid(1), knob.name().mid(1));
    if (!QFile::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='34' height='20' viewBox='0 0 34 20'>"
                                   "<rect x='0.5' y='0.5' width='33' height='19' rx='9.5' fill='%1' fill-opacity='%2'/>"
                                   "<circle cx='%3' cy='10' r='8' fill='%4'/></svg>")
                        .arg(track.name(QColor::HexRgb)).arg(track.alphaF(), 0, 'f', 3)
                        .arg(on ? 24 : 10).arg(knob.name(QColor::HexRgb))
                        .toUtf8());
    }
    return path;
}

QString hex(const QColor &c)
{
    return c.alpha() == 255 ? c.name() : QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

// Fusion, minus the dotted/outlined focus frame item views draw around the current row.
class FlatStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const override
    {
        if (pe == PE_FrameFocusRect && w && (qobject_cast<const QAbstractItemView *>(w) ||
                                             qobject_cast<const QAbstractItemView *>(w->parentWidget())))
            return;
        QProxyStyle::drawPrimitive(pe, opt, p, w);
    }
};

const char *kStyleSheet = R"QSS(
* { outline: none; }
QMainWindow, QDialog { background: {bg}; }
QWidget { color: {text}; }

/* toolbar */
QWidget#mainToolbar { background: {bg}; border-bottom: 1px solid {separator}; }
QWidget#sidebarPanel { background: {sidebarBg}; }
QWidget#statusStrip { background: {bg}; border-top: 1px solid {separator}; }
QWidget#termHeader { background: {bg}; border-top: 1px solid {separator}; }
QSplitter#vsplit::handle { background: transparent; }
QToolButton#flat { border: none; border-radius: 7px; padding: 5px; background: transparent; }
QToolButton#flat:hover { background: {hover}; }
QToolButton#flat:pressed { background: {selInactive}; }
QToolButton#flat::menu-indicator { image: none; width: 0; }
QFrame#segmented { background: {inputBg}; border-radius: 8px; }
QFrame#segmented QToolButton { border: none; border-radius: 6px; padding: 4px 9px; background: transparent; }
QFrame#segmented QToolButton:hover { background: {hover}; }
QFrame#segmented QToolButton:checked { background: {segmentChecked}; }
QLineEdit#search { background: {inputBg}; border: 1px solid transparent; border-radius: 8px; padding: 5px 6px; selection-background-color: {accent}; }
QLineEdit#search:focus { border: 1px solid {accent}; }
QFrame#aiPopup { background: {menuBg}; border: 1px solid {separator}; border-radius: 12px; }
QLineEdit#aiPrompt { background: transparent; border: none; padding: 4px 2px; font-size: 15px; }

/* breadcrumb */
QToolButton#crumb { border: none; border-radius: 6px; padding: 3px 6px; color: {secondary}; background: transparent; }
QToolButton#crumb:hover { background: {hover}; color: {text}; }
QToolButton#crumbCurrent { border: none; border-radius: 6px; padding: 3px 6px; color: {text}; font-weight: 600; background: transparent; }
QToolButton#crumbCurrent:hover { background: {hover}; }
QLabel#crumbSep { color: {tertiary}; }
QLineEdit#pathEdit { background: {inputBg}; border: 1px solid {accent}; border-radius: 7px; padding: 4px 8px; }

/* sidebar */
QTreeWidget#sidebar { background: {sidebarBg}; border: none; padding: 0 8px 8px 8px; }
QTreeWidget#sidebar::branch { background: transparent; image: none; border: none; }

/* file views */
QAbstractItemView#files { background: {bg}; border: none; selection-background-color: {selection}; selection-color: {selText}; }
QTreeView#files { padding: 2px 8px; show-decoration-selected: 0; }
QTreeView#files::item { border: none; padding: 0 4px; }
QTreeView#files { alternate-background-color: transparent; }
QTreeView#files::item:alternate { background: transparent; }
/* row backgrounds (stripes, hover, selection) are painted as one pill in FileTreeView::drawRow */
QTreeView#files::item:hover { background: transparent; }
QTreeView#files::item:selected:active { background: transparent; color: {selText}; }
QTreeView#files::item:selected:!active { background: transparent; color: {text}; }
QTreeView#files::branch { background: transparent; border: none; image: none; }
QTreeView#files::branch:selected { background: transparent; }
QTreeView#files::branch:has-children:closed { image: url({branchClosed}); }
QTreeView#files::branch:has-children:open { image: url({branchOpen}); }
QTreeView#files::branch:has-children:closed:selected:active { image: url({branchClosedSel}); }
QTreeView#files::branch:has-children:open:selected:active { image: url({branchOpenSel}); }
QColumnView#files { border: none; background: {bg}; }
QColumnView#files QListView { background: {bg}; border: none; border-right: 1px solid {separator}; padding: 4px 6px; }
QColumnView#files QListView::item { border: none; border-radius: 7px; padding: 0 4px; }
QColumnView#files QListView::item:hover { background: {hover}; }
QColumnView#files QListView::item:selected:active { background: {selection}; color: {selText}; }
QColumnView#files QListView::item:selected:!active { background: {selInactive}; color: {text}; }
/* folder tree (`), over the file views */
QFrame#folderTreePanel { background: {bg}; }
QLineEdit#folderTreeQuery { padding: 5px 8px; }
QListView#folderTree { background: {bg}; border: none; }
QLabel#folderTreeKeys { color: {secondary}; padding: 4px 4px 2px 4px; }
QToolButton#folderTreeCase { border: none; border-radius: 6px; padding: 4px 7px; color: {secondary}; font-weight: 600; }
QToolButton#folderTreeCase:hover { background: {hover}; }
QToolButton#folderTreeCase:checked { background: {accent}; color: {selText}; }
QHeaderView { background: {bg}; border: none; }
QHeaderView::section { background: {bg}; color: {secondary}; border: none; border-bottom: 1px solid {separator}; padding: 6px 10px; font-weight: 500; }
QHeaderView::section:hover { color: {text}; }

/* scrollbars */
QScrollBar:vertical { background: transparent; width: 11px; margin: 2px 1px; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 1px 2px; }
QScrollBar::handle:vertical { background: {scroll}; border-radius: 3px; min-height: 32px; margin: 0 2px; }
QScrollBar::handle:horizontal { background: {scroll}; border-radius: 3px; min-width: 32px; margin: 2px 0; }
QScrollBar::handle:hover { background: {scrollHover}; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* tabs */
QTabWidget::pane { border: none; }
QTabWidget::tab-bar { left: 0; }
QTabBar { background: {bg}; }
QTabBar::tab { background: transparent; color: {secondary}; border: none; border-radius: 7px; padding: 5px 14px; margin: 6px 2px 6px 2px; min-width: 110px; }
QTabBar::tab:hover { background: {hover}; color: {text}; }
QTabBar::tab:selected { background: {inputBg}; color: {text}; }
QTabBar::close-button { image: url({tabClose}); subcontrol-position: right; width: 16px; height: 16px; border-radius: 4px; }
QTabBar::close-button:hover { image: url({tabCloseHover}); background: {hover}; }
QTabBar#termTabs { background: transparent; }
QTabBar#termTabs::tab { padding: 2px 6px 2px 10px; margin: 3px 2px 3px 0; min-width: 70px; max-width: 220px; border-radius: 6px; }
QWidget#tabBarStrip { background: {bg}; border-bottom: 1px solid {separator}; }

/* status bar */
QSlider::groove:horizontal { height: 4px; background: {separator}; border-radius: 2px; }
QSlider::sub-page:horizontal { background: {accent}; border-radius: 2px; }
QSlider::handle:horizontal { width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; background: #ffffff; border: 1px solid {separator}; }
QProgressBar { background: {separator}; border: none; border-radius: 3px; max-height: 6px; }
QProgressBar::chunk { background: {accent}; border-radius: 3px; }

QSplitter::handle { background: {separator}; }
QToolButton#flat:disabled { background: transparent; }
QSplitter::handle:horizontal { width: 1px; }

/* menus */
QMenu { background: {menuBg}; border: 1px solid {separator}; border-radius: 8px; padding: 5px; }
QMenu::item { padding: 5px 26px 5px 12px; border-radius: 5px; }
QMenu::icon { subcontrol-origin: content; subcontrol-position: left center; position: relative; left: -5px; } /* centered between the edge and the label */
QMenu::item:selected { background: {accent}; color: {selText}; }
QMenu::item:disabled { color: {tertiary}; }
QMenu::separator { height: 1px; background: {separator}; margin: 4px 8px; }

/* dialogs & controls */
QPushButton { background: {inputBg}; border: none; border-radius: 7px; padding: 6px 16px; min-width: 64px; }
QPushButton:hover { background: {selInactive}; }
QPushButton:default, QPushButton#primary { background: {accent}; color: {selText}; }
QPushButton#primary:hover { background: {accent}; }
QPushButton[small="true"] { padding: 2px 10px; min-width: 0; border-radius: 5px; font-size: 11px; }
QLineEdit { background: {inputBg}; border: 1px solid transparent; border-radius: 6px; padding: 4px 6px; selection-background-color: {accent}; }
QLineEdit:focus { border: 1px solid {accent}; }
QLineEdit#renameEdit { padding: 0; border-radius: 4px; }
QPlainTextEdit { background: {bg}; border: none; selection-background-color: {accent}; }
QLabel#warning { color: {danger}; font-size: 11px; }
QPlainTextEdit#commandText { background: {inputBg}; border: 1px solid transparent; border-radius: 6px; padding: 4px; }
QPlainTextEdit#commandText:focus { border: 1px solid {accent}; }
QToolTip { background: {menuBg}; color: {text}; border: 1px solid {separator}; border-radius: 6px; padding: 4px 6px; }
QLabel#secondary { color: {secondary}; }
QCheckBox#switch { spacing: 10px; }
QCheckBox#switch::indicator { width: 34px; height: 20px; image: url({switchOff}); }
QCheckBox#switch::indicator:checked { image: url({switchOn}); }
QListWidget#settingsNav { background: {sidebarBg}; border: none; padding: 12px 8px; }
QListWidget#settingsNav::item { border-radius: 7px; padding: 4px 6px; color: {text}; }
QListWidget#settingsNav::item:selected { background: {selInactive}; color: {text}; }
QComboBox { background: {inputBg}; border: none; border-radius: 7px; padding: 4px 10px; min-width: 170px; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: url({branchOpen}); }
QComboBox QAbstractItemView { background: {menuBg}; border: 1px solid {separator}; selection-background-color: {accent}; outline: none; }
QSpinBox { background: {inputBg}; border: none; border-radius: 7px; padding: 4px 8px; }
QLabel#title { font-size: 17px; font-weight: 600; }
)QSS";

} // namespace

void Theme::ShortcutMenu::paintEvent(QPaintEvent *event)
{
    QMenu::paintEvent(event);
    QPainter painter(this);
    for (QAction *action : actions()) {
        QStyleOptionMenuItem option;
        initStyleOption(&option, action);
        if (!option.text.contains(QLatin1Char('\t')))
            continue;
        const QRect row = actionGeometry(action);
        const qreal dpr = devicePixelRatioF();
        auto render = [&](bool shortcut) {
            QImage image(row.size() * dpr, QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Theme::colors().menuBg);
            QStyleOptionMenuItem item(option);
            item.rect = QRect(QPoint(), row.size());
            if (!shortcut)
                item.text = item.text.left(item.text.indexOf(QLatin1Char('\t')) + 1);
            QPainter p(&image);
            p.setFont(font());
            style()->drawControl(QStyle::CE_MenuItem, &item, &p, this);
            return image;
        };
        // Use the menu's complete style, including QSS, which bypasses the base proxy style.
        QImage full = render(true), bare = render(false);
        for (int y = 0; y < full.height(); ++y) {
            auto *a = reinterpret_cast<QRgb *>(full.scanLine(y));
            const auto *b = reinterpret_cast<const QRgb *>(bare.constScanLine(y));
            for (int x = 0; x < full.width(); ++x)
                a[x] = qRgba((qRed(a[x]) + qRed(b[x])) / 2, (qGreen(a[x]) + qGreen(b[x])) / 2,
                             (qBlue(a[x]) + qBlue(b[x])) / 2, (qAlpha(a[x]) + qAlpha(b[x])) / 2);
        }
        painter.drawImage(row.topLeft(), full);
    }
}


Theme *Theme::instance()
{
    static Theme *t = new Theme;
    return t;
}

namespace {
void repaintFileViews();
}

void Theme::install()
{
    // Grab the system accent before Fusion replaces the platform palette.
    m_systemAccent = QGuiApplication::palette().color(QPalette::Accent);
    if (!m_systemAccent.isValid() || m_systemAccent == QColor(Qt::black))
        m_systemAccent = QColor(0x0A, 0x84, 0xFF);
    QApplication::setStyle(new FlatStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    apply();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { apply(); });
    connect(Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String(Settings::ThemeMode))
            apply();
        if (key == QLatin1String(Settings::FileColors) || key == QLatin1String(Settings::FolderColor) ||
            key == QLatin1String(Settings::BoldNames) || key == QLatin1String(Settings::UppercaseNames) ||
            key == QLatin1String(Settings::FileColorSelection) || key.isEmpty())
            QTimer::singleShot(0, this, repaintFileViews); // after fileColorTable() has dropped the old colors
    });
}

void Theme::apply()
{
    // A fixed light/dark choice in Settings overrides the system (also tints the native title bar).
    const QString mode = Settings::instance()->value(Settings::ThemeMode).toString();
    auto *hints = QGuiApplication::styleHints();
    const Qt::ColorScheme wanted = mode == QLatin1String("light") ? Qt::ColorScheme::Light
                                 : mode == QLatin1String("dark")  ? Qt::ColorScheme::Dark
                                                                  : Qt::ColorScheme::Unknown;
    if (wanted == Qt::ColorScheme::Unknown)
        hints->unsetColorScheme();
    else if (hints->colorScheme() != wanted)
        hints->setColorScheme(wanted);
    const bool dark = wanted == Qt::ColorScheme::Dark
                      || (wanted == Qt::ColorScheme::Unknown && hints->colorScheme() == Qt::ColorScheme::Dark);
    Colors c;
    c.dark = dark;
    c.accent = m_systemAccent;
    // Windows' system accent is especially luminous in a full selected row. Keep the accent for
    // controls, but make only selection highlights a touch quieter.
#ifdef Q_OS_WIN
    c.selection = c.accent.darker(135);
#else
    c.selection = c.accent;
#ifdef Q_OS_LINUX
    if (dark)
        c.selection = QColor(qRound(c.accent.red() * 0.68), qRound(c.accent.green() * 0.68),
                             qRound(c.accent.blue() * 0.68), c.accent.alpha());
#endif
#endif
    c.selText = QColor(Qt::white);
    c.nameSelection = nameSelectionColor(c.accent, dark);
    if (dark) {
        c.bg = QColor(0x1E, 0x1E, 0x20);
        c.sidebarBg = QColor(0x26, 0x26, 0x29);
        c.text = QColor(0xF2, 0xF2, 0xF4);
        c.secondary = QColor(0x9A, 0x9A, 0xA0);
        c.tertiary = QColor(0x6A, 0x6A, 0x70);
        c.separator = QColor(0x34, 0x34, 0x38);
        c.selInactive = QColor(0x3A, 0x3A, 0x3E);
        c.hover = QColor(255, 255, 255, 14);
        c.inputBg = QColor(0x2C, 0x2C, 0x30);
        c.segmentChecked = QColor(0x55, 0x55, 0x5B);
        c.menuBg = QColor(0x2A, 0x2A, 0x2E);
        c.scroll = QColor(255, 255, 255, 60);
        c.scrollHover = QColor(255, 255, 255, 110);
        c.tile = QColor(255, 255, 255, 22);
        c.altRow = QColor(255, 255, 255, 9);
        c.danger = QColor(255, 69, 58); // systemRed (dark)
    } else {
        c.bg = QColor(0xFF, 0xFF, 0xFF);
        c.sidebarBg = QColor(0xF5, 0xF5, 0xF7);
        c.text = QColor(0x1D, 0x1D, 0x1F);
        c.secondary = QColor(0x80, 0x80, 0x86);
        c.tertiary = QColor(0xB0, 0xB0, 0xB6);
        c.separator = QColor(0xE6, 0xE6, 0xEA);
        c.selInactive = QColor(0xE2, 0xE2, 0xE7);
        c.hover = QColor(0, 0, 0, 10);
        c.inputBg = QColor(0xEF, 0xEF, 0xF2);
        c.segmentChecked = QColor(0xFF, 0xFF, 0xFF);
        c.menuBg = QColor(0xFF, 0xFF, 0xFF);
        c.scroll = QColor(0, 0, 0, 55);
        c.scrollHover = QColor(0, 0, 0, 100);
        c.tile = QColor(0, 0, 0, 14);
        c.altRow = QColor(0, 0, 0, 7);
        c.danger = QColor(215, 0, 21); // systemRed (light)
    }
    m_c = c;

    QPalette pal;
    pal.setColor(QPalette::Window, c.bg);
    pal.setColor(QPalette::Base, c.bg);
    pal.setColor(QPalette::AlternateBase, c.sidebarBg);
    pal.setColor(QPalette::WindowText, c.text);
    pal.setColor(QPalette::Text, c.text);
    pal.setColor(QPalette::ButtonText, c.text);
    pal.setColor(QPalette::Button, c.inputBg);
    pal.setColor(QPalette::Highlight, c.selection);
    pal.setColor(QPalette::HighlightedText, c.selText);
    pal.setColor(QPalette::Accent, c.accent);
    pal.setColor(QPalette::PlaceholderText, c.secondary);
    pal.setColor(QPalette::ToolTipBase, c.menuBg);
    pal.setColor(QPalette::ToolTipText, c.text);
    pal.setColor(QPalette::Light, c.separator);
    pal.setColor(QPalette::Midlight, c.separator);
    pal.setColor(QPalette::Mid, c.separator);
    pal.setColor(QPalette::Dark, c.separator);
    pal.setColor(QPalette::Shadow, QColor(0, 0, 0, 60));
    pal.setColor(QPalette::Disabled, QPalette::Text, c.tertiary);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, c.tertiary);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, c.tertiary);
    QApplication::setPalette(pal);

    QString qss = QString::fromUtf8(kStyleSheet);
    const QHash<QString, QColor> vars = {
        {"bg", c.bg}, {"sidebarBg", c.sidebarBg}, {"text", c.text}, {"secondary", c.secondary},
        {"tertiary", c.tertiary}, {"separator", c.separator}, {"accent", c.accent}, {"selection", c.selection}, {"selText", c.selText},
        {"selInactive", c.selInactive}, {"hover", c.hover}, {"inputBg", c.inputBg},
        {"segmentChecked", c.segmentChecked}, {"menuBg", c.menuBg}, {"scroll", c.scroll},
        {"scrollHover", c.scrollHover}, {"altRow", c.altRow}, {"danger", c.danger},
    };
    for (auto it = vars.begin(); it != vars.end(); ++it)
        qss.replace(QLatin1Char('{') + it.key() + QLatin1Char('}'), hex(it.value()));
    qss.replace(QStringLiteral("{branchClosedSel}"), glyphFile(QStringLiteral("chevron-right-small"), c.selText));
    qss.replace(QStringLiteral("{branchOpenSel}"), glyphFile(QStringLiteral("chevron-down-small"), c.selText));
    qss.replace(QStringLiteral("{branchClosed}"), glyphFile(QStringLiteral("chevron-right-small"), c.secondary));
    qss.replace(QStringLiteral("{branchOpen}"), glyphFile(QStringLiteral("chevron-down-small"), c.secondary));
    qss.replace(QStringLiteral("{tabClose}"), glyphFile(QStringLiteral("close-small"), c.secondary));
    qss.replace(QStringLiteral("{tabCloseHover}"), glyphFile(QStringLiteral("close-small"), c.text));
    qss.replace(QStringLiteral("{switchOn}"), switchFile(true, c.accent, Qt::white));
    qss.replace(QStringLiteral("{switchOff}"), switchFile(false, dark ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 30), Qt::white));
    qApp->setStyleSheet(qss);
    emit changed();
}

QIcon Theme::icon(const QString &name, const QColor &color, int size)
{
    const QString body = glyphs().value(name);
    if (body.isEmpty())
        return {};
    const Colors &c = colors();
    const QColor normal = color.isValid() ? color : c.text;
    // Cached per appearance (apply() clears it) so delegates can ask for icons while painting.
    static QHash<QString, QIcon> cache;
    static bool cacheDark = c.dark;
    if (cacheDark != c.dark) {
        cache.clear();
        cacheDark = c.dark;
    }
    const QString key = name + QLatin1Char('|') + normal.name(QColor::HexArgb) + QLatin1Char('|') + QString::number(size);
    if (auto it = cache.constFind(key); it != cache.constEnd())
        return *it;
    QIcon icon;
    for (qreal dpr : {1.0, 2.0}) {
        icon.addPixmap(renderGlyph(body, normal, size, dpr), QIcon::Normal, QIcon::Off);
        icon.addPixmap(renderGlyph(body, normal, size, dpr), QIcon::Normal, QIcon::On);
        icon.addPixmap(renderGlyph(body, c.tertiary, size, dpr), QIcon::Disabled, QIcon::Off);
        icon.addPixmap(renderGlyph(body, c.selText, size, dpr), QIcon::Selected, QIcon::Off); // on the selection color
    }
    cache.insert(key, icon);
    return icon;
}

namespace {
// The color picker's preview (Theme::previewFileColor / previewFolderColor), over the settings.
QHash<QString, QColor> previewExtensions;
QColor previewFolder;

void repaintFileViews()
{
    for (QWidget *w : QApplication::allWidgets())
        if (auto *v = qobject_cast<QAbstractItemView *>(w))
            v->viewport()->update();
}

// Extension -> its color as given (dark mode), from config.toml file_colors.groups; rebuilt when it changes.
QHash<QString, QColor> &fileColorTable()
{
    static QHash<QString, QColor> table;
    static bool built = false;
    if (!built) {
        built = true;
        QObject::connect(Settings::instance(), &Settings::changed, Settings::instance(), [](const QString &key) {
            if (key == QLatin1String(Settings::FileColors) || key.isEmpty()) {
                table.clear();
                built = false;
            }
        });
        for (const QVariant &row : Settings::instance()->value(Settings::FileColors).toList()) {
            const QVariantMap m = row.toMap();
            const QColor c(m.value(QStringLiteral("color")).toString());
            for (const QString &ext : util::extensionList(m.value(QStringLiteral("extensions")).toString()))
                if (!table.contains(ext)) // the first group naming an extension wins
                    table.insert(ext, c);
        }
    }
    return table;
}
} // namespace

QColor Theme::fileBaseColor(const QString &fileName)
{
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == fileName.size() - 1)
        return {};
    const QString ext = fileName.mid(dot + 1).toLower();
    return previewExtensions.contains(ext) ? previewExtensions.value(ext) : fileColorTable().value(ext);
}

QColor Theme::fileColor(const QString &fileName)
{
    const QColor c = fileBaseColor(fileName);
    if (!c.isValid() || colors().dark)
        return c;
    return lightModeColor(c);
}

QColor Theme::plainSelectionColor() { return QColor(0xC4, 0xC4, 0xCA); } // light gray, as for dark mode
QColor Theme::selectionTextColor() { return QColor(0x14, 0x14, 0x16); }  // one black on every item color

namespace {
// OKLab (Björn Ottosson): lightness as the eye sees it, so colors of different hues can be given the
// same visual weight; HSL's lightness is not (yellow at 50% looks far brighter than blue at 50%).
struct OkLch {
    double l, c, h; // h in radians
};
double toLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double fromLinear(double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }

OkLch toOkLch(const QColor &c)
{
    const double r = toLinear(c.redF()), g = toLinear(c.greenF()), b = toLinear(c.blueF());
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    return {L, std::hypot(A, B), std::atan2(B, A)};
}

// Linear sRGB of an OKLCH color; inGamut tells whether it fits.
std::array<double, 3> fromOkLch(const OkLch &o, bool *inGamut)
{
    const double A = o.c * std::cos(o.h), B = o.c * std::sin(o.h);
    const double l = std::pow(o.l + 0.3963377774 * A + 0.2158037573 * B, 3);
    const double m = std::pow(o.l - 0.1055613458 * A - 0.0638541728 * B, 3);
    const double s = std::pow(o.l - 0.0894841775 * A - 1.2914855480 * B, 3);
    const std::array<double, 3> rgb{4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                                    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                                    -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
    *inGamut = std::all_of(rgb.begin(), rgb.end(), [](double v) { return v >= -1e-4 && v <= 1 + 1e-4; });
    return rgb;
}
} // namespace

QColor Theme::lightModeColor(const QColor &c)
{
    // Colors are chosen against the dark background. On white every one gets the same perceived
    // lightness (OKLab L 0.52: the contrast of the darkest kinds), its own hue and a little more
    // chroma, cut back to what sRGB can show at that lightness. HSL scaling left yellow and cyan
    // light enough to vanish on white while blue went near black.
    OkLch o = toOkLch(c);
    o.l = 0.52;
    double lo = 0, hi = o.c * 1.1;
    bool fits = false;
    fromOkLch({o.l, hi, o.h}, &fits);
    if (!fits) { // the most chroma that fits, by bisection
        for (int i = 0; i < 24; ++i) {
            const double mid = (lo + hi) / 2;
            fromOkLch({o.l, mid, o.h}, &fits);
            (fits ? lo : hi) = mid;
        }
        hi = lo;
    }
    bool ignored = false;
    const auto rgb = fromOkLch({o.l, hi, o.h}, &ignored);
    return QColor::fromRgbF(float(fromLinear(qBound(0.0, rgb[0], 1.0))), float(fromLinear(qBound(0.0, rgb[1], 1.0))),
                            float(fromLinear(qBound(0.0, rgb[2], 1.0))));
}

QColor Theme::selectionFill(const QColor &color)
{
    OkLch o = toOkLch(color);
    o.l *= 0.8;
    bool fits = false;
    auto rgb = fromOkLch(o, &fits);
    for (int i = 0; i < 24 && !fits; ++i) { // darker can leave the gamut for vivid hues: ease chroma
        o.c *= 0.95;
        rgb = fromOkLch(o, &fits);
    }
    return QColor::fromRgbF(float(fromLinear(qBound(0.0, rgb[0], 1.0))), float(fromLinear(qBound(0.0, rgb[1], 1.0))),
                            float(fromLinear(qBound(0.0, rgb[2], 1.0))));
}

QColor Theme::nameSelectionColor(const QColor &accent, bool dark)
{
    OkLch o = toOkLch(accent);
    o.l = dark ? 0.42 : 0.88;
    if (!dark)
        o.c *= 0.35;
    bool fits = false;
    auto rgb = fromOkLch(o, &fits);
    for (int i = 0; i < 40 && !fits; ++i) {
        o.c *= 0.95;
        rgb = fromOkLch(o, &fits);
    }
    return QColor::fromRgbF(float(fromLinear(qBound(0.0, rgb[0], 1.0))), float(fromLinear(qBound(0.0, rgb[1], 1.0))),
                            float(fromLinear(qBound(0.0, rgb[2], 1.0))));
}

void Theme::previewFileColor(const QStringList &extensions, const QColor &darkModeColor)
{
    previewExtensions.clear();
    if (darkModeColor.isValid())
        for (const QString &e : extensions)
            previewExtensions.insert(e, darkModeColor);
    repaintFileViews();
}

void Theme::previewFolderColor(const QColor &darkModeColor)
{
    previewFolder = darkModeColor;
    repaintFileViews();
}

QColor Theme::folderBaseColor()
{
    const QString hex = Settings::instance()->value(Settings::FolderColor).toString();
    if (hex.isEmpty() && !previewFolder.isValid())
        return {}; // the normal text color
    return previewFolder.isValid() ? previewFolder : QColor(hex);
}

QColor Theme::folderColor()
{
    const QColor c = folderBaseColor();
    return !c.isValid() || colors().dark ? c : lightModeColor(c);
}
