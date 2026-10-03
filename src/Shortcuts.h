#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>

#include "Util.h"

class QAction;
class QKeyEvent;

// Application shortcuts, including contextual view/preview/terminal keys. Defaults are in Shortcuts.cpp
// (in menu order); the user's own keys live in config.toml's [shortcuts] table (Settings) and are
// applied to every window at once, including after the file is edited outside the app.
class Shortcuts : public QObject {
    Q_OBJECT
public:
    struct Entry {
        QString id;    // the action's menu title in Korean (the source text): stable across languages
        QString group; // its menu (also Korean)
        QString title() const { return Gifiles::tr(id.toUtf8().constData()); }
        QList<QKeySequence> defaults;
        QString context; // empty = window; otherwise keys are local to this UI area
    };

    static Shortcuts *instance();
    static const QList<Entry> &entries();
    // The id an older name in config.toml [shortcuts] stands for now (the id itself if not renamed).
    static QString currentId(const QString &id);

    // Gives a window's action its keys (the user's, else the built-in ones) and keeps it updated.
    void add(QAction *action, const QString &id);

    QList<QKeySequence> keys(const QString &id) const;
    QList<QKeySequence> defaults(const QString &id) const;
    bool isCustom(const QString &id) const;
    QString context(const QString &id) const;
    bool conflicts(const QString &first, const QString &second) const;
    bool matches(const QString &id, const QKeyEvent *event) const;
    // Gives `key` to `id` alone and takes it away from any other action; returns those actions' ids.
    QStringList assign(const QString &id, const QKeySequence &key);
    void reset(const QString &id);
    void resetAll();               // every action back to its built-in keys
    void clearAll();               // no key for any action

    static QStringList toStrings(const QList<QKeySequence> &keys);

signals:
    void changed();

private:
    Shortcuts();
    void store(const QString &id, const QList<QKeySequence> &keys);
    void applyAll();

    QMultiHash<QString, QPointer<QAction>> m_actions;
};
