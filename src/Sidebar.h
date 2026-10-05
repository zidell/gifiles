#pragma once

#include <QTreeWidget>
#include <QUrl>

// Favorites, recent folders (RecentFolders: where the user did something) and mounted volumes.
// Dropping files/folders between favorites adds them as favorites at that spot (or moves an
// existing favorite there); dropping onto a folder row moves/copies into it.
class Sidebar : public QTreeWidget {
    Q_OBJECT
public:
    explicit Sidebar(QWidget *parent = nullptr);

    void setCurrentPath(const QString &path);
    static QStringList favorites();
    static QStringList defaultFavorites(); // home, the user folders (and /Applications on macOS)
    static void setFavorites(const QStringList &paths);
    // Inserts paths at `index` among the favorites (existing entries are moved there).
    void insertFavorites(const QStringList &paths, int index);

signals:
    void placeActivated(const QString &path);
    void rightPressed(); // → moves the keyboard to the file view
    void openInNewTab(const QString &path);
    // mods: the keys held at the drop (it is handled later, when they may be up already).
    void dropRequested(const QList<QUrl> &urls, const QString &targetDir, Qt::DropAction action, Qt::KeyboardModifiers mods);

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    struct DropTarget {
        enum Kind { None, Insert, Into } kind = None;
        int index = -1;     // Insert: position among favorites
        QString intoPath;   // Into: folder or volume to drop files into
        int lineY = -1;     // Insert: where to draw the insertion line
    };
    DropTarget dropTargetAt(const QPoint &pos, bool internal) const;
    QStringList shownFavorites() const;
    void clearDropFeedback();
    void rebuild();
    void refreshVolumes();
    QTreeWidgetItem *addPlace(QTreeWidgetItem *section, const QString &path, const QString &label = {});

    QList<QTreeWidgetItem *> sections() const;

    QTreeWidgetItem *m_favorites = nullptr;
    QTreeWidgetItem *m_recent = nullptr; // only while there are recent folders to show
    QTreeWidgetItem *m_locations = nullptr;
    QStringList m_volumeRoots;
    QString m_current;
    int m_dropLineY = -1;
    QPoint m_pressPos;
    QString m_pressFavorite;
};
