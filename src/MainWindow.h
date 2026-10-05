#pragma once

#include "BrowserTab.h"
#include "FileOps.h"
#include "Settings.h"

#include <QMainWindow>
#include <QPointer>
#include <QUrl>
#include <QVariantMap>

class QAction;
class QActionGroup;
class QLabel;
class QLineEdit;
class QProgressBar;
class QSlider;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QMenu;
class QTimer;
class QToolButton;
class PathBar;
class PreviewWidget;
class QuickLookWindow;
class Sidebar;
class FolderTreePanel;
class TerminalWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QStringList &tabPaths, QWidget *parent = nullptr);

    BrowserTab *tab() const;
    BrowserTab *addTab(const QString &path, bool activate = true, bool atEnd = false); // else after the current tab
    QVariantMap saveState() const;
    void restoreState(const QVariantMap &state);
    // A "선택한 항목들로…" command: asks for {prompt} if it has one, then runs it in the terminal.
    // Runs a "선택한 항목들로…" command (config.toml [selection_menu]) on these items: asks for
    // {prompt} first if it has one; in the terminal, or quietly when its terminal = false.
    void runSelectionCommand(const QVariantMap &command, const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void showEvent(QShowEvent *e) override;

private:
    void createActions();
    void createMenus();
    void createToolbar();
    void createStatusBar();
    QToolButton *flatButton(QAction *action, const QString &glyph, QWidget *parent);
    void refreshIcons();
    void updateToolbarInset();
    void showMessage(const QString &text, int ms);
    QAction *act(const QString &text, void (MainWindow::*slot)());
    QAction *act(const QString &text, std::function<void()> fn);

    void connectTab(BrowserTab *t);
    void onTabChanged();
    void forgetPlaybackOutside(const QString &folder);
    void updateNav();
    void updateStatus();
    void updateTabTitle(BrowserTab *t);
    void updateUndoActions();
    void closeTab(int index);
    void go(const QString &path);
    void setMode(BrowserTab::Mode m);
    void focusSidebar();

    // File operations
    void newFolder();
    void newFolderWithSelection();
    void duplicate();
    void moveToTrash();
    void copyToClipboard(bool cut);
    void paste(bool forceMove);
    void copyPath();
    void undo();
    void redo();
    void rename(const QString &path, const QString &newName);
    void handleDrop(const QList<QUrl> &urls, const QString &targetDir, Qt::DropAction proposed, Qt::KeyboardModifiers mods);
    void runTransfer(const QStringList &sources, const QString &destDir, bool move);
    void runJob(const Job &job, const QString &selectAfter = {});
    bool resolveConflicts(const QStringList &sources, const QString &destDir, QHash<QString, Conflict> *out);

    void toggleQuickLook();
    void toggleFolderTree(); // ` : the NCD-style tree of every folder
    void placeFolderTree();
    // keepHeight: unfolding by dragging the edge, the panel keeps the dragged height.
    void setTerminalVisible(bool visible, bool keepHeight = false);
    QString debugState() const; // one line for the debug log: folder, view, selection, focus
    // Terminal tabs (Chrome-like, in the panel's bar). term() is the current one, or none yet.
    TerminalWidget *term() const;
    TerminalWidget *addTerminal(const QString &cwd);
    void closeTerminal(int index);
    void updateTerminalTab(TerminalWidget *t);
    bool terminalHasFocus() const;
    // The browser moved: the current terminal cd's along when idle; when a program is running in
    // it, a new terminal tab opens at the folder instead.
    void terminalFollow(const QString &path);
    // AI button: asks for a one-line request and runs the configured CLI (claude -p, codex exec)
    // in the terminal panel with the request and the selected items' paths.
    void runCommandNow(const QVariantMap &command, const QStringList &paths, const QString &prompt);
    void runQuietly(const QString &label, const QString &command, const QString &dir);
    bool closeFrontWindow();
    // One line of input in a small popup under `anchor`; `done` gets the text on Enter.
    void askLine(const QString &placeholder, const QString &note, std::function<void(const QString &)> done);
    void runInTerminal(const QString &command);
    void showInfo();
    void revealInFileManager(); // Finder / Explorer / the desktop's file manager
    void checkForUpdates();     // 업데이트 확인…: the result in a message box
    void restartToUpdate();     // install the downloaded version now (the app quits and comes back)
    void showContextMenu(const QPoint &globalPos, bool onItem, bool fromKey = false);

    QTabWidget *m_tabs;
    QWidget *m_toolbar = nullptr;
    QWidget *m_toolbarInset = nullptr;
    QWidget *m_sidebarPanel = nullptr;
    QWidget *m_sidebarTop = nullptr;
    QWidget *m_statusStrip = nullptr;
    QAction *m_searchIcon = nullptr;
    QTimer *m_messageTimer = nullptr;
    QList<QPair<QPointer<QToolButton>, QString>> m_themed;
    QList<QMenu *> m_menus;
    QSplitter *m_vsplit = nullptr;       // file views above, terminal below
    QWidget *m_termPanel = nullptr;
    QStackedWidget *m_termStack = nullptr; // one TerminalWidget per tab, same order as m_termTabs
    QTabBar *m_termTabs = nullptr;
    QLabel *m_termTitle = nullptr; // "터미널" until the first shell starts
    QWidget *m_termHeader = nullptr;
    QToolButton *m_termToggle = nullptr;
    QToolButton *m_termAdd = nullptr; // "+": hidden while the terminal is folded
    QToolButton *m_updateButton = nullptr; // shown once a new version is downloaded and ready
    int m_termHeight = 0; // last unfolded height of the terminal panel
    bool m_termOpen = false;
    void setTerminalHeight(int h);
    static constexpr int kTermHeaderHeight = 28;
    QAction *m_terminalAct = nullptr;
    Sidebar *m_sidebar;
    PreviewWidget *m_previewPane;
    QSplitter *m_splitter;
    PathBar *m_pathBar;
    QLineEdit *m_search;
    QLabel *m_statusLabel;
    QProgressBar *m_progress;
    QToolButton *m_cancel;
    QSlider *m_iconSlider;
    QuickLookWindow *m_quickLook = nullptr;
    QPointer<QWidget> m_quickLookTarget;
    QPointer<BrowserTab> m_connectedTab;
    int m_runningJobs = 0;
    QList<std::shared_ptr<std::atomic<bool>>> m_jobCancels; // the running jobs' flags: 중단 stops them all
    // ⌘Z / ⇧⌘Z pressed while a job runs (true = undo): they wait for its undo record, or they
    // would undo the operation before it.
    QList<bool> m_deferredUndo;

    QAction *m_back, *m_forward, *m_up;
    QAction *m_modeList, *m_modeGallery, *m_modeColumns;
    QActionGroup *m_modeGroup;
    QAction *m_undo, *m_redo;
    QAction *m_showHidden, *m_showSidebar, *m_showPreview;
    QAction *m_quickLookAct, *m_revealAct, *m_openAct, *m_openTabAct, *m_renameAct, *m_duplicateAct, *m_trashAct, *m_infoAct;
    QAction *m_cutAct, *m_copyAct, *m_pasteAct, *m_moveHereAct, *m_copyPathAct, *m_selectAllAct;
    QAction *m_permissionsAct, *m_newFolderWithAct, *m_settingsAct, *m_openWithAct, *m_updateAct;
    QAction *m_newWindowAct, *m_newTabAct, *m_newFolderAct, *m_closeTabAct, *m_closeWindowAct, *m_quitAct;
    QAction *m_findAct, *m_gotoAct, *m_biggerAct, *m_smallerAct, *m_nextTabAct, *m_prevTabAct;
    QAction *m_fontBiggerAct, *m_fontSmallerAct, *m_fontResetAct;
    QAction *m_focusSidebarAct, *m_focusFilesAct, *m_focusTerminalAct;
    QList<QAction *> m_goPlaces;
    QAction *m_folderTreeAct = nullptr;
    FolderTreePanel *m_folderTree = nullptr;
};
