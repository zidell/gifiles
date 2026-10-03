#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

// The file system's record of which folders changed (macOS FSEvents; nothing elsewhere): live, and
// since a saved point, also for the time the app wasn't running. The folder tree's index follows it
// instead of being scanned again.
namespace FolderEvents {

bool available();
quint64 currentId();                          // where the journal stands now
QString journalId(const QStringList &roots);  // the volumes' journals; "" if one has none (network)

struct Batch {
    QStringList changed; // folders whose subfolders may have changed (paths as under `roots`)
    QStringList deep;    // folders to read again with everything inside (events were dropped)
    quint64 lastId = 0;
    bool historyDone = false; // everything since `since` has been delivered
    bool reset = false;       // the journal can't be trusted any more: scan again
};

class Stream {
public:
    // `callback` runs on the GUI thread, never after the Stream is gone.
    Stream(const QStringList &roots, quint64 since, std::function<void(const Batch &)> callback);
    ~Stream();
    struct Private;

private:
    std::unique_ptr<Private> d;
};

} // namespace FolderEvents
