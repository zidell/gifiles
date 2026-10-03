#pragma once

#include <QCollator>
#include <QFileSystemModel>
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <QUrl>

#include <functional>

// Columns in the order Finder shows them; mapped onto QFileSystemModel's columns.
enum Column { ColName = 0, ColSize = 1, ColKind = 2, ColDate = 3 };

// Sorting (folders first, natural order), name search, Korean labels, and drop routing
// on top of QFileSystemModel.
class FileProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit FileProxy(QFileSystemModel *fs, QObject *parent = nullptr);

    QFileSystemModel *fs() const { return m_fs; }
    QString filePath(const QModelIndex &proxyIndex) const;
    bool isDir(const QModelIndex &proxyIndex) const;
    QModelIndex indexForPath(const QString &path) const;

    // The folder whose children are filtered by the search text.
    void setSearchRoot(const QString &path);
    void setSearch(const QString &text);
    QString search() const { return m_search; }

    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool hasChildren(const QModelIndex &parent) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                         const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;
    bool removeRows(int, int, const QModelIndex &) override { return false; }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction; }
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction | Qt::MoveAction; }

signals:
    // Files were dropped onto targetDir; the window decides copy vs move and runs the job.
    void dropRequested(const QList<QUrl> &urls, const QString &targetDir, Qt::DropAction action);

protected:
    bool lessThan(const QModelIndex &l, const QModelIndex &r) const override;
    bool filterAcceptsRow(int row, const QModelIndex &sourceParent) const override;

private:
    void refilter(const std::function<void()> &change);
    QString dropTarget(const QModelIndex &parent) const;

    QFileSystemModel *m_fs;
    QCollator m_collator;
    QString m_search;
    QPersistentModelIndex m_searchRoot;
};
