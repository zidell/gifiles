#pragma once

#include "FileOps.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>

class MainWindow;

// Application-wide state: open windows, the shared undo history, global view preferences.
class App : public QObject {
    Q_OBJECT
public:
    static App *instance();

    MainWindow *newWindow(const QStringList &tabPaths = {}, QWidget *near = nullptr);
    QList<MainWindow *> windows() const; // open ones, oldest first
    void restoreSession();
    void saveSession(const QList<MainWindow *> &windows);
    void windowClosing(MainWindow *w);
    void quit();

    // Items cut with ⌘X/Ctrl+X, shown dimmed until pasted (Windows-style move).
    void setCutPaths(const QStringList &paths);
    bool isCut(const QString &path) const { return m_cut.contains(path); }

    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    // A non-blocking notice listing what was wrong in config.toml (the old values stay).
    void showConfigProblems(const QStringList &problems);

    // Undo history (Finder keeps one for the whole app).
    void recordDone(const UndoRecord &rec);    // a new user action: clears redo
    void recordUndone(const UndoRecord &rec);  // pushes onto redo
    void recordRedone(const UndoRecord &rec);  // pushes onto undo, keeps redo
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QString undoLabel() const { return canUndo() ? m_undo.last().label : QString(); }
    QString redoLabel() const { return canRedo() ? m_redo.last().label : QString(); }
    UndoRecord takeUndo();
    UndoRecord takeRedo();

signals:
    void showHiddenChanged(bool show);
    void undoChanged();
    void cutChanged();

private:
    explicit App(QObject *parent = nullptr);
    QList<QPointer<MainWindow>> m_windows;
    QList<UndoRecord> m_undo, m_redo;
    bool m_showHidden = false;
    QSet<QString> m_cut;
    bool m_quitting = false;
};
