#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <atomic>
#include <vector>

// Every folder under some roots (the whole drive by default), for the NCD-style folder tree (`):
// a compact tree in breadth-first order, so the children of a folder are consecutive and sorted
// (natural order). Names are kept twice in UTF-8 arenas: as they are and folded (NFC, case-folded)
// for matching. About 30 bytes a folder; 340 000 folders scan in ~7 s on a Mac SSD, network and
// FUSE mounts excluded (one NFS mount alone took 5 minutes) — see docs/history.md.
class FolderIndex {
public:
    struct Options {
        QStringList roots;   // absolute folders, "/" separators
        // Folder names (wildcards, matched anywhere) or absolute paths (start with / ~ or a drive
        // letter; wildcards allowed) to leave out together with everything inside.
        QStringList exclude;
        bool skipPackages = false; // macOS: .app and other bundles are files, not folders
        QString key() const;       // what a cached index depends on
    };

    // Lists the folders; `progress` counts them as they come, `cancel` stops early (empty result).
    static FolderIndex scan(const Options &options, std::atomic<int> *progress = nullptr,
                            const std::atomic<bool> *cancel = nullptr);
    // Mount points under the roots that are network, FUSE or virtual file systems (left out).
    static QStringList foreignMounts();

    int size() const { return int(m_nodes.size()); }
    int rootCount() const { return m_roots; }
    int parent(int n) const { return m_nodes[n].parent; }
    int firstChild(int n) const { return m_nodes[n].first; }
    int childCount(int n) const { return m_nodes[n].count; }
    int depth(int n) const { return m_nodes[n].depth; }
    QString name(int n) const;
    QString path(int n) const;
    int find(const QString &path) const; // the node of a folder, -1 if it isn't in the index
    int findNearest(const QString &path) const; // that folder or its closest indexed ancestor

    // The folders whose name contains the query's last part (case- and normalization-blind); a
    // query like "si/gif" also wants an earlier ancestor to contain "si". Best first: exact name,
    // name prefix, word start, anywhere; then by `boost` (visits per node), then shallower.
    struct Matches {
        std::vector<int> best; // at most `limit`
        int total = 0;
    };
    Matches match(const QString &query, const QHash<int, int> &boost = {}, int limit = 2000) const;

    bool save(const QString &file, const QString &key, qint64 scannedAt) const;
    // False if the file is missing, damaged or was made for other options.
    static bool load(const QString &file, const QString &key, FolderIndex &out, qint64 *scannedAt = nullptr);

private:
    struct Node {
        qint32 parent;
        qint32 first;
        qint32 count;
        quint32 nameOff;
        quint32 foldOff;
        quint16 nameLen;
        quint16 foldLen;
        quint16 depth;
    };
    QByteArrayView folded(int n) const { return QByteArrayView(m_folded.constData() + m_nodes[n].foldOff, m_nodes[n].foldLen); }
    int childNamed(int parent, const QByteArray &foldedName) const;

    std::vector<Node> m_nodes;
    QByteArray m_names, m_folded;
    int m_roots = 0;
};
