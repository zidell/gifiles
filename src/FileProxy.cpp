#include "FileProxy.h"
#include "Settings.h"
#include "Util.h"

#include <QMimeData>

namespace {
// Natural order by hand, for the C/POSIX locale, where QCollator ignores numeric mode (e.g. Linux
// sessions with LANG=C.UTF-8): digit runs compare by value, the rest case-insensitively.
int naturalCompare(QStringView a, QStringView b)
{
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].isDigit() && b[j].isDigit()) {
            qsizetype ie = i, je = j;
            while (ie < a.size() && a[ie].isDigit())
                ++ie;
            while (je < b.size() && b[je].isDigit())
                ++je;
            QStringView na = a.sliced(i, ie - i), nb = b.sliced(j, je - j);
            while (na.size() > 1 && na.front() == u'0')
                na = na.sliced(1);
            while (nb.size() > 1 && nb.front() == u'0')
                nb = nb.sliced(1);
            if (na.size() != nb.size())
                return na.size() < nb.size() ? -1 : 1;
            if (const int c = na.compare(nb))
                return c;
            i = ie;
            j = je;
            continue;
        }
        const QChar ca = a[i].toCaseFolded(), cb = b[j].toCaseFolded();
        if (ca != cb)
            return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size() || j < b.size())
        return i < a.size() ? 1 : -1;
    return a.compare(b);
}
} // namespace

FileProxy::FileProxy(QFileSystemModel *fs, QObject *parent) : QSortFilterProxyModel(parent), m_fs(fs)
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    setSourceModel(fs);
    setDynamicSortFilter(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

QString FileProxy::filePath(const QModelIndex &idx) const
{
    return idx.isValid() ? m_fs->filePath(mapToSource(idx)) : QString();
}

bool FileProxy::isDir(const QModelIndex &idx) const
{
    return idx.isValid() && m_fs->isDir(mapToSource(idx));
}

QModelIndex FileProxy::indexForPath(const QString &path) const
{
    return mapFromSource(m_fs->index(path));
}

void FileProxy::refilter(const std::function<void()> &change)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    change();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    change();
    invalidateRowsFilter();
#endif
}

void FileProxy::setSearchRoot(const QString &path)
{
    const QPersistentModelIndex root(m_fs->index(path));
    if (m_search.isEmpty())
        m_searchRoot = root;
    else
        refilter([&] { m_searchRoot = root; });
}

void FileProxy::setSearch(const QString &text)
{
    if (text == m_search)
        return;
    refilter([&] { m_search = text; });
}

bool FileProxy::filterAcceptsRow(int row, const QModelIndex &sourceParent) const
{
    if (m_search.isEmpty() || sourceParent != m_searchRoot)
        return true;
    const QString name = m_fs->fileName(m_fs->index(row, 0, sourceParent));
    return name.contains(m_search, Qt::CaseInsensitive);
}

bool FileProxy::lessThan(const QModelIndex &l, const QModelIndex &r) const
{
    const bool ld = m_fs->isDir(l), rd = m_fs->isDir(r);
    if (ld != rd && Settings::instance()->flag(Settings::FoldersFirst)) // whichever direction we sort in
        return sortOrder() == Qt::AscendingOrder ? ld : rd;
    int c = 0;
    switch (l.column()) {
    case ColSize:
        if (!ld)
            c = m_fs->size(l) < m_fs->size(r) ? -1 : m_fs->size(l) > m_fs->size(r) ? 1 : 0;
        break;
    case ColKind:
        c = m_collator.compare(util::kindOf(m_fs->fileInfo(l)), util::kindOf(m_fs->fileInfo(r)));
        break;
    case ColDate: {
        const QDateTime a = m_fs->lastModified(l), b = m_fs->lastModified(r);
        c = a < b ? -1 : a > b ? 1 : 0;
        break;
    }
    default:
        break;
    }
    if (c == 0)
        c = m_collator.locale().language() == QLocale::C ? naturalCompare(m_fs->fileName(l), m_fs->fileName(r))
                                                         : m_collator.compare(m_fs->fileName(l), m_fs->fileName(r));
    return c < 0;
}

QVariant FileProxy::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const int col = index.column();
    if (role == Qt::DisplayRole && col != ColName) {
        const QModelIndex src = mapToSource(index);
        switch (col) {
        case ColSize:
            return m_fs->isDir(src) ? QStringLiteral("--") : util::humanSize(m_fs->size(src));
        case ColKind:
            return util::kindOf(m_fs->fileInfo(src));
        case ColDate:
            return util::humanDate(m_fs->lastModified(src));
        }
    }
    if (role == Qt::TextAlignmentRole && col == ColSize)
        return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole && col == ColName)
        return QDir::toNativeSeparators(filePath(index));
    return QSortFilterProxyModel::data(index, role);
}

// App bundles and other packages are opened, not browsed: no disclosure chevron, no column.
bool FileProxy::hasChildren(const QModelIndex &parent) const
{
    if (parent.isValid() && util::isPackage(m_fs->fileInfo(mapToSource(parent))))
        return false;
    return QSortFilterProxyModel::hasChildren(parent);
}

bool FileProxy::canFetchMore(const QModelIndex &parent) const
{
    if (parent.isValid() && util::isPackage(m_fs->fileInfo(mapToSource(parent))))
        return false;
    return QSortFilterProxyModel::canFetchMore(parent);
}

Qt::ItemFlags FileProxy::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QSortFilterProxyModel::flags(index);
    // QFileSystemModel only marks items editable after it has read their permissions in the
    // background, so a rename right after opening a folder would be ignored. Always allow it;
    // FileOps::rename reports permission errors.
    if (index.isValid() && index.column() == ColName)
        f |= Qt::ItemIsEditable;
    return f;
}

QVariant FileProxy::headerData(int section, Qt::Orientation o, int role) const
{
    if (o == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case ColName: return Gifiles::tr("이름");
        case ColSize: return Gifiles::tr("크기");
        case ColKind: return Gifiles::tr("종류");
        case ColDate: return Gifiles::tr("수정일");
        }
    }
    if (o == Qt::Horizontal && role == Qt::TextAlignmentRole)
        return QVariant::fromValue((section == ColSize ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    return QSortFilterProxyModel::headerData(section, o, role);
}

QString FileProxy::dropTarget(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return {};
    const QModelIndex src = mapToSource(parent);
    if (!m_fs->isDir(src) || util::isPackage(m_fs->fileInfo(src)))
        return {};
    return m_fs->filePath(src);
}

bool FileProxy::canDropMimeData(const QMimeData *data, Qt::DropAction, int, int, const QModelIndex &parent) const
{
    if (!data->hasUrls() || dropTarget(parent).isEmpty())
        return false;
    const QString target = dropTarget(parent);
    for (const QUrl &u : data->urls()) {
        if (!u.isLocalFile())
            return false;
        // Can't drop a folder into itself or onto itself.
        if (util::isInside(target, u.toLocalFile()))
            return false;
    }
    return true;
}

bool FileProxy::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                             const QModelIndex &parent)
{
    if (!canDropMimeData(data, action, row, column, parent))
        return false;
    emit dropRequested(data->urls(), dropTarget(parent), action);
    // Returning false keeps the source view from trying to remove the dragged rows itself.
    return false;
}
