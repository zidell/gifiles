#pragma once

#include <QStringList>
#include <QList>

// External terminals are opened only on an explicit user request, never while following folders.
namespace TerminalLauncher {
struct App {
    QString id, name, program;
};
struct Command {
    QString program;
    QStringList arguments;
    QString directory;
};
struct Shell {
    QString name, program;
};
// Shells the built-in terminal can run; on Windows the installed PowerShells, newest first.
QList<Shell> shells();
// terminal.shell, or else Windows' newest PowerShell / the login shell ($SHELL).
QString defaultShell();
QList<App> available();
Command command(const App &app, const QString &directory, const QString &line);
bool launch(const QString &directory, const QString &line = {});
}
