#pragma once

#include <QObject>
#include <QStringList>

struct UndoRecord;

// The sidebar's "최근 폴더": folders the user worked in, not just looked at. A folder counts once
// something was done there: items copied, moved, made, renamed, trashed or extracted into it (every
// new undo record, App::recordDone), a file opened from it, a "선택한 항목들로…" command run on its
// items, or a command typed in a terminal at it (cd, ls and other moves or looks don't count).
// Newest first. A list of state, kept in QSettings like the session; how many show is config.toml's
// sidebar.recent_folders.
class RecentFolders : public QObject {
    Q_OBJECT
public:
    static RecentFolders *instance();

    void note(const QString &folder);
    void noteRecord(const UndoRecord &rec);
    // A line run in a terminal at `folder`; `line` is what the user typed (empty when it came back
    // from the shell's history, which counts as work).
    void noteCommand(const QString &folder, const QString &line);
    void remove(const QString &folder);
    void clear();

    // Existing folders, newest first, as many as config.toml's sidebar.recent_folders allows.
    QStringList folders() const;
    // Commands that only move around or look: cd, pushd, ls, pwd, clear, … (PowerShell's too).
    static bool isNavigationCommand(const QString &line);

    static constexpr int kKept = 50; // stored (shown: the setting, at most this)

signals:
    void changed();

private:
    RecentFolders();
    QStringList stored() const;
    void store(const QStringList &list);
    void noteAll(const QStringList &folders);
};
