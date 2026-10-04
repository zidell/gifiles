#pragma once

#include <QModelIndex>
#include <QUrl>
#include <QElapsedTimer>
#include <QWidget>

#include <optional>

class QAbstractItemView;
class QColumnView;
class QFileSystemModel;
class QFileSystemWatcher;
class QListView;
class QStackedWidget;
class QTreeView;
class QTimer;
class FileProxy;
class ItemDelegate;
class PreviewWidget;

// One tab: a folder shown as list, gallery (icon grid with thumbnails) or columns,
// with its own back/forward history.
class BrowserTab : public QWidget {
    Q_OBJECT
public:
    enum Mode { List = 0, Gallery = 1, Columns = 2 };

    BrowserTab(const QString &path, Mode mode, bool showHidden, QWidget *parent = nullptr);

    QString path() const { return m_path; }
    Mode mode() const { return m_mode; }
    // remember: store as this folder's view (Finder remembers mode, sort, columns per folder).
    void setMode(Mode mode, bool remember = false);

    // Shows `path` (a folder; a file path shows its folder with the file selected).
    void navigate(const QString &path, bool pushHistory = true, const QStringList &select = {});
    bool canGoBack() const { return m_histIndex > 0; }
    bool canGoForward() const { return m_histIndex < m_history.size() - 1; }
    void goBack();
    void goForward();
    void goUp();

    QStringList selectedPaths() const;
    QString currentItemPath() const;
    // The item to select once the current selection is removed (next unselected sibling, else previous).
    QString neighborOfSelection() const;
    // Selects items as soon as they appear in the model (e.g. right after a copy finishes).
    void selectPaths(const QStringList &paths, bool thenRename = false);
    void selectAll();
    void renameSelected();
    void openSelection(bool inNewTab);
    bool hasSelectedFolder() const; // a folder (not a package) is among the selected items
    // Where a menu for the selection opens (below the current item), in global coordinates.
    QPoint selectionMenuPos() const;

    void setShowHidden(bool show);
    void setSearch(const QString &text);
    void setIconSize(int size, bool remember = false);
    int iconSize() const { return m_iconSize; }

    int itemCount() const;
    QAbstractItemView *view() const;
    void focusView(bool selectFirstIfEmpty = false);

signals:
    void pathChanged(const QString &path);
    void modeChanged();
    void selectionChanged();
    void currentItemChanged(const QString &path);
    void quickLookRequested();
    void openInNewTabRequested(const QString &path);
    void extractRequested(const QStringList &archives);
    void dropRequested(const QList<QUrl> &urls, const QString &targetDir, Qt::DropAction action);
    void renameRequested(const QString &path, const QString &newName);
    void contextMenuRequested(const QPoint &globalPos, bool onItem);
    void leftEdgeReached(); // ← in the list with nothing left to fold or climb: the sidebar's turn

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void setPathInternal(const QString &path, bool pushHistory);
    void applyRoot();
    void checkRoot(); // the open folder's row went away (renamed, replaced): show it again, or its parent
    void relistIfStale(); // the open folder changed but the model didn't follow: make it list and watch again
    void syncColumns();
    void trySelectPending();
    void openIndex(const QModelIndex &idx, bool inNewTab);
    void attachView(QAbstractItemView *v);
    QAbstractItemView *columnFor(const QModelIndex &parent) const;
    void selectIndexes(const QModelIndexList &idxs);
    void onCurrentChanged(const QModelIndex &current);
    void applyFolderPrefs();
    void saveFolderPrefs();
    bool listRight(bool recursive);
    bool listLeft(bool recursive);

    QFileSystemModel *m_fs;
    FileProxy *m_proxy;
    QStackedWidget *m_stack;
    QTreeView *m_list;
    QListView *m_gallery;
    QColumnView *m_columns;
    PreviewWidget *m_columnPreview;
    ItemDelegate *m_listDelegate;
    ItemDelegate *m_galleryDelegate;
    ItemDelegate *m_columnDelegate;

    Mode m_mode = List;
    QString m_path;
    QStringList m_history;
    int m_histIndex = -1;
    QStringList m_pending;
    // Just-selected new items: the file system model may drop and re-add a fresh row (Linux,
    // inotify), which loses the selection; it is restored when that happens shortly after.
    QStringList m_justSelected;
    QElapsedTimer m_justSelectedAt;
    QString m_enterWhenLoaded; // column view: folder whose first item to select once it has loaded
    bool m_pendingRename = false;
    bool m_syncingColumns = false;
    int m_iconSize = 96;
    bool m_applyingPrefs = false;
    QTimer *m_prefsTimer = nullptr;
    QTimer *m_rootGone = nullptr; // the open folder stayed missing: go to its parent
    QFileSystemWatcher *m_folderWatch = nullptr; // the open folder, besides the model's own watching
    QTimer *m_staleCheck = nullptr;
};
