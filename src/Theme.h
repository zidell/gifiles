#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QMenu>

// Flat, modern look shared by every window: colors follow the system light/dark setting and
// accent color; icons are line glyphs drawn from embedded SVG and tinted to the palette.
class Theme : public QObject {
    Q_OBJECT
public:
    class ShortcutMenu : public QMenu {
    public:
        using QMenu::QMenu;
    protected:
        void paintEvent(QPaintEvent *event) override;
    };

    struct Colors {
        bool dark;
        QColor bg, sidebarBg, text, secondary, tertiary, separator, accent, selection, nameSelection, selText, selInactive,
            hover, inputBg, segmentChecked, menuBg, scroll, scrollHover, tile, altRow, danger;
    };

    static Theme *instance();
    static const Colors &colors() { return instance()->m_c; }

    // Applies style, palette and stylesheet to the application; re-applies on appearance changes.
    void install();

    // name: "chevron-left", "list", "grid", "columns", "search", "plus", "sidebar", "preview",
    // "home", "apps", "desktop", "documents", "downloads", "pictures", "music", "movies",
    // "folder", "drive", "chevron-right-small", "shield", "more".
    static QIcon icon(const QString &name, const QColor &color = {}, int size = 18);
    // Text color for a file name by its extension (config.toml file_colors.groups, given for dark
    // mode; light mode gets lightModeColor of it). Invalid for names without an extension and for
    // extensions that aren't listed.
    static QColor fileColor(const QString &fileName);
    static QColor lightModeColor(const QColor &darkModeColor);
    // The colors as given (for dark mode) in either theme: the selection background uses them.
    static QColor fileBaseColor(const QString &fileName);
    static QColor folderBaseColor();
    // Selection in the items' colors (file_colors.selection): items without a color get this light
    // gray; the text on any of them is this black (Mdir's selection bar).
    static QColor plainSelectionColor();
    // The selection bar's fill for an item color: the same hue at 80% of its perceived lightness,
    // so the bar doesn't glare and the black text on it reads well.
    static QColor selectionFill(const QColor &color);
    // The one selection bar under names that keep their own colors: the accent's hue, deep in dark
    // mode (names are light there) and pale in light mode (names are dark there).
    static QColor nameSelectionColor(const QColor &accent, bool dark);
    static QColor selectionTextColor();
    // The color picker's live preview: these extensions (or folders) show `darkModeColor` until it's
    // called with an invalid color; repaints the file views.
    static void previewFileColor(const QStringList &extensions, const QColor &darkModeColor);
    static void previewFolderColor(const QColor &darkModeColor);
    // Folder names (bold): config.toml file_colors.folder, light mode as above; invalid = normal text color.
    static QColor folderColor();

signals:
    void changed();

private:
    Theme() = default;
    void apply();
    Colors m_c{};
    QColor m_systemAccent;
};
