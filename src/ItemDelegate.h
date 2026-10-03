#pragma once

#include <QElapsedTimer>
#include <QStyledItemDelegate>

#include <optional>

class FileProxy;

// Draws file items for each view (list rows, gallery tiles with thumbnails, column rows with a
// folder chevron) and runs Finder-style renames: the editor starts with the name minus extension
// selected, and committing calls back into the window instead of writing to the model.
class ItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    enum Kind { List, Gallery, Column };
    ItemDelegate(FileProxy *proxy, Kind kind, QObject *parent);

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override;
    void setEditorData(QWidget *editor, const QModelIndex &index) const override;
    // The text of a rename editor that went away without Return or Esc in the last two seconds
    // (its row was removed and re-added: Linux's file watcher does that to a new folder), once.
    std::optional<QString> takeLostEdit(const QString &path) const;
    // The background of a selected item in an active view when config.toml file_colors.selection is
    // on: its file/folder color (as given, both themes), light gray without one; invalid when off.
    QColor selectionColor(const QModelIndex &index) const;

signals:
    void renameRequested(const QString &path, const QString &newName) const;

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override;
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void paintItem(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const;
    bool isFolder(const QModelIndex &index) const; // a real folder, not a package (.app)
    // A name's color (folder blue, the file's kind, else `plain`); bolds `font` for folders.
    QColor nameStyle(const QModelIndex &index, const QColor &plain, QFont *font) const;
    QPixmap thumbnail(const QModelIndex &index) const;
    void paintTile(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const;
    QRect tileRect(const QStyleOptionViewItem &opt) const;

    FileProxy *m_proxy;
    Kind m_kind;
    struct LostEdit {
        QString path, text;
        QElapsedTimer at;
    };
    mutable std::optional<LostEdit> m_lost;
};
