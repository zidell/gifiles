#pragma once

#include <QFuture>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

// One primitive file-system change, recorded so it can be undone/redone.
struct Step {
    enum Kind { Move, Copy, Trash, Mkdir, Extract } kind;
    QString from; // Move/Copy: source, Trash: original location, Extract: archive
    QString to;   // Move/Copy: destination, Trash: location inside the trash, Mkdir: created folder,
                  // Extract: the extracted item

};

struct UndoRecord {
    QString label; // "이동", "복사", ...
    QList<Step> steps;
    bool isEmpty() const { return steps.isEmpty(); }
};

enum class Conflict { KeepBoth, Replace, Skip };

struct Job {
    enum Type { Copy, Move, Trash, Duplicate, Extract, Undo, Redo } type = Copy;
    QStringList sources;
    QString destDir;
    QHash<QString, Conflict> conflicts; // keyed by source path, only for names that already exist in destDir
    UndoRecord record;                  // Undo/Redo only
    // Set from the UI to stop early; work done so far is still reported (and undoable).
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
};

struct OpResult {
    Job::Type type = Job::Copy;
    QString destDir;
    QStringList created; // paths that now exist and should be selected
    QStringList errors;
    bool canceled = false;
    UndoRecord record; // what was done (for the undo stack)
    UndoRecord rest;   // Undo/Redo canceled part-way: the steps not replayed, back to the stack they came from
};

namespace FileOps {

// Runs a job on the thread pool. Progress text is the item being processed.
QFuture<OpResult> start(const Job &job);
QString verb(Job::Type t); // "복사", "이동", ...

// Archives Gifiles expands itself (next to the archive, like Archive Utility) instead of
// handing them to the system, which would reveal the result in Finder.
bool isArchive(const QString &path);

// Small synchronous operations. Return an error message, or an empty string on success.
QString rename(const QString &path, const QString &newName, UndoRecord *rec);
QString makeFolder(const QString &dir, const QString &name, QString *created, UndoRecord *rec);
// Finder's "선택 항목으로 새로운 폴더": a new folder in dir holding the given items (same volume).
QString makeFolderWith(const QString &dir, const QStringList &items, QString *created, UndoRecord *rec);

} // namespace FileOps
