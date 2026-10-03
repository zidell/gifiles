#pragma once

#include <QDialog>
#include <QObject>
#include <QVariant>

class QFileSystemWatcher;

// User preferences, stored in config.toml (TOML with a description above every item, so the file
// alone explains itself — see packaging/readme.txt and docs/history.md). Keys are "table/name".
// The app writes the whole file on every change and re-reads it when it changes on disk, so an
// edit made outside the app applies at once; a wrong value is reported and the old one kept.
class Settings : public QObject {
    Q_OBJECT
public:
    static Settings *instance();

    QVariant value(const QString &key) const; // falls back to the default for known keys
    void setValue(const QString &key, const QVariant &v);
    bool contains(const QString &key) const;  // set in the file (for open_with/ and shortcuts/)
    void remove(const QString &key);
    QStringList keys(const QString &table) const; // keys set in an open-ended table (open_with)
    bool flag(const QString &key) const { return value(key).toBool(); }

    // Keys
    static constexpr const char *ThemeMode = "appearance/theme";              // "system" | "light" | "dark"
    static constexpr const char *BoldNames = "appearance/bold_names";            // bool: every name bold, not only folders
    static constexpr const char *UppercaseNames = "appearance/uppercase_names";  // bool: names shown in capitals
    static constexpr const char *Stripes = "view/stripes";                    // bool
    static constexpr const char *FoldersFirst = "view/folders_first";         // bool
    static constexpr const char *DefaultMode = "view/default_mode";           // "list" | "gallery" | "columns"
    static constexpr const char *ShowHidden = "view/show_hidden";             // bool
    static constexpr const char *IconSize = "view/icon_size";                 // gallery icon size, px
    static constexpr const char *FontSize = "view/font_size";                 // file views, pt; 0 = system
    static constexpr const char *QuickLookScale = "preview/quick_look_scale"; // % of the natural size
    static constexpr const char *QuickLookDocScale = "preview/quick_look_doc_scale"; // % of the base window size and text
    // Quick Look window size the user chose by dragging (or + / − on sound), px; 0 = the base size.
    static constexpr const char *QuickLookDocWidth = "preview/quick_look_doc_width";
    static constexpr const char *QuickLookDocHeight = "preview/quick_look_doc_height";
    static constexpr const char *QuickLookAudioWidth = "preview/quick_look_audio_width";
    static constexpr const char *QuickLookAudioHeight = "preview/quick_look_audio_height";
    static constexpr const char *MediaVolume = "preview/volume";              // audio/video volume, %
    static constexpr const char *Language = "general/language";               // "system" | "ko" | "en" | "ja" | "zh_CN"
    static constexpr const char *RestoreSession = "general/restore_session";
    static constexpr const char *AutoUpdate = "general/auto_update";
    static constexpr const char *NewWindowHome = "general/new_window_opens_home"; // true: home, false: current folder
    static constexpr const char *DontAskFullDisk = "general/skip_full_disk_access_prompt";
    static constexpr const char *TermShell = "terminal/shell";                // empty = system default
    static constexpr const char *TermFontSize = "terminal/font_size";
    static constexpr const char *TermFollowFolder = "terminal/follow_folder";
    static constexpr const char *TermSyncBack = "terminal/sync_back";
    static constexpr const char *PreviewTextFontSize = "preview/text_font_size"; // code, config: fixed-width
    static constexpr const char *PreviewDocFontSize = "preview/doc_font_size";   // txt, md: UI font
    static constexpr const char *Favorites = "sidebar/favorites";             // folder paths
    // [{id?, label, key, terminal, command}], the context menu's "선택한 항목들로…"; the built-in ones
    // (id "new_folder", "zip", "ai") can be changed but not removed.
    static constexpr const char *SelectionCommands = "selection_menu/commands";
    // [{extensions, color}]: file name colors by extension ("zip, 7z", "#DC88DC"), given for dark mode;
    // light mode uses a darker, stronger version of each (Theme::fileColor).
    static constexpr const char *FileColors = "file_colors/groups";
    static constexpr const char *FileColorSelection = "file_colors/selection"; // bool: selection in the item's color
    static constexpr const char *FolderColor = "file_colors/folder"; // "#RRGGBB", dark mode as above; "" = text color
    // The folder tree (`): folders to list ([] = the whole drive: "/", every fixed drive on Windows)
    // and what to leave out (folder names with wildcards, or absolute paths).
    static constexpr const char *FolderTreeRoots = "folder_tree/roots";
    static constexpr const char *FolderTreeExclude = "folder_tree/exclude";
    // "open_with/<extension>": app remembered by "항상 이 앱으로 열기"; "shortcuts/<menu title>": keys.

    // A "선택한 항목들로…" command by its id ("ai": the toolbar's AI button).
    QVariantMap selectionCommand(const QString &id) const;
    static QVariantList defaultFileColors();

    // config.toml: where it is (GIFILES_CONFIG_DIR overrides the folder), the file as the app
    // would write it now (or with every value at its default), and a check of a file's contents.
    static QString configPath();
    QString render() const;
    static QString renderDefaults();
    static QString renderEffective(QStringList &problems); // what a start now would use: the file, defaults for the rest
    static QStringList check(const QString &text); // problems, empty if the file is fine
    // The UI language to load ("ko", "en", "ja", "zh_CN"): general.language read straight from the
    // file (no Settings instance: usable before the app's windows and by the command-line options),
    // "system" resolved from the system's languages; English for languages without a translation.
    static QString uiLanguage();
    QStringList problems() const { return m_problems; } // from the last load

signals:
    void changed(const QString &key); // "" after a reload that may have changed anything
    void problemsFound(const QStringList &problems);

private:
    Settings();
    void load(bool initial);
    void save();

    QMap<QString, QVariant> m_values; // "table/name" -> value, only what the file sets
    QStringList m_problems;
    int m_saveRetries = 0; // a save that failed (file busy on Windows) is tried again
    QByteArray m_written; // the file as we last wrote or read it, to tell outside edits from our own
    QFileSystemWatcher *m_watcher = nullptr;
};

// ⌘, preferences window: categories on the left, switches and pickers on the right.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    static void showSingleton(QWidget *parent, const QString &page = {}); // page: its name in the list

private:
    explicit SettingsDialog(QWidget *parent);
};
