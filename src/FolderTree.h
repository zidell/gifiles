#pragma once

#include "FolderIndex.h"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QLineEdit>
#include <QTimer>

#include <memory>

class QLabel;
class QListView;

// The NCD-style folder tree (` in the file views): every folder of the drive in a tree; typing
// jumps to the best match, Tab / ⇧Tab to the next ones, arrows walk the tree, Return goes there.
// The tree is always fully unfolded, like Norton's NCD.
//
// FolderTree keeps the index: loaded from the cache file while a panel is open (freed two minutes
// after the last one closes), rescanned in the background when it is old, when a file operation
// changed folders, or when the folder the panel opens on changed since the scan.
class FolderTree : public QObject {
    Q_OBJECT
public:
    static FolderTree *instance();
    // config.toml folder_tree.* (+ on macOS without Full Disk Access, the protected folders the
    // user hasn't opened yet: scanning them would make macOS ask).
    static FolderIndex::Options options();
    static QStringList defaultExclude(); // config.toml default, per platform

    std::shared_ptr<const FolderIndex> index() const { return m_index; }
    bool isScanning() const { return m_scanning; }
    int scanned() const { return m_progress->load(); }
    qint64 scannedAt() const { return m_scannedAt; } // ms since the epoch

    // A panel opens on `folder`: load the cache; rescan if it is old, a file operation changed
    // folders, or `folder` changed since the scan.
    void acquire(const QString &folder);
    void release(); // a panel closed
    void prefetch(); // at start (main.cpp, not the tests): keep the cache fresh in the background
    void markStale();
    void noteVisit(const QString &path); // the browser showed this folder (ranking, macOS access)
    QHash<int, int> boost() const;       // visits per node of the current index

signals:
    void indexChanged();
    void progress(int folders); // while scanning

private:
    FolderTree();
    void startScan(bool lowPriority);
    void saveVisits();
    static QString cacheFile(const QString &key);

    std::shared_ptr<const FolderIndex> m_index;
    QString m_indexKey;
    qint64 m_scannedAt = 0;
    bool m_stale = false, m_scanning = false, m_staleDuringScan = false;
    int m_users = 0;
    std::shared_ptr<std::atomic<int>> m_progress = std::make_shared<std::atomic<int>>(0);
    QTimer m_release, m_progressTimer, m_saveVisits, m_prefetch;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false); // at quit
    QHash<QString, int> m_visits;
};

// Every folder of a FolderIndex as one fully unfolded tree (NCD): a flat list in depth-first order,
// so any match is one row away; FolderTreeDelegate draws the indentation and the tree lines.
class FolderTreeModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;
    void setIndex(std::shared_ptr<const FolderIndex> index);
    const FolderIndex *folders() const { return m_index.get(); }
    QModelIndex indexOf(int node) const;
    int nodeOf(const QModelIndex &index) const
    {
        return index.isValid() && size_t(index.row()) < m_order.size() ? m_order[size_t(index.row())] : -1;
    }
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

private:
    std::shared_ptr<const FolderIndex> m_index;
    std::vector<int> m_order, m_rowOf; // row -> node, node -> row
};

// The query line: its text plus what the input method is still composing (Korean), so a match
// follows every jamo typed.
class FolderTreeQuery : public QLineEdit {
    Q_OBJECT
public:
    using QLineEdit::QLineEdit;
    QString query() const { return text() + m_preedit; }
signals:
    void queryChanged();

protected:
    void inputMethodEvent(QInputMethodEvent *e) override;

private:
    QString m_preedit;
};

// Laid over the file views of a window (MainWindow places it); closes on Esc / ` / a click away.
class FolderTreePanel : public QFrame {
    Q_OBJECT
public:
    explicit FolderTreePanel(QWidget *parent);
    void open(const QString &current);
    void dismiss();
    QString currentPath() const; // the folder under the cursor
    QListView *view() const { return m_view; }
    // Just closed by ` typed through an input method: the same key then reaches the file view,
    // which must not open the panel again.
    bool justDismissed() const { return m_imeClosed.isValid() && m_imeClosed.elapsed() < 300; }
    FolderTreeQuery *queryEdit() const { return m_edit; }

signals:
    void chosen(const QString &path);
    void dismissed();

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void takeIndex();
    void runQuery(bool jump);
    void jumpTo(int node);
    void moveTo(int node); // the cursor, without touching the matches
    void step(int delta);
    void choose();
    void updateStatus();

    FolderTreeQuery *m_edit;
    QListView *m_view;
    FolderTreeModel *m_model;
    QLabel *m_status;
    std::shared_ptr<const FolderIndex> m_index;
    QHash<int, int> m_boost;
    FolderIndex::Matches m_matches;
    int m_matchPos = -1;
    QString m_current;
    QElapsedTimer m_imeClosed;
};
