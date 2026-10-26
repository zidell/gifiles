#include "TerminalLauncher.h"
#include "Settings.h"
#include "Util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace TerminalLauncher {
QList<Shell> shells()
{
    QList<Shell> list;
    QStringList seen;
    auto add = [&](const QString &name, const QString &program) {
        const QString key = QFileInfo(program).canonicalFilePath().toLower();
        if (program.isEmpty() || seen.contains(key))
            return;
        seen << key;
        list.append({name, QDir::toNativeSeparators(program)});
    };
#ifdef Q_OS_WIN
    // The built-in terminal starts PowerShell (prompt function, quoting), so only PowerShells.
    const QString programFiles = qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"));
    auto first = [](const QStringList &candidates) {
        for (const QString &c : candidates) {
            if (QFileInfo(c).isAbsolute()) {
                if (QFileInfo::exists(c))
                    return c;
            } else if (const QString found = QStandardPaths::findExecutable(c); !found.isEmpty()) {
                return found;
            }
        }
        return QString();
    };
    add(QStringLiteral("PowerShell 7 Preview"),
        first({programFiles + QStringLiteral("/PowerShell/7-preview/pwsh.exe"), QStringLiteral("pwsh-preview")}));
    add(QStringLiteral("PowerShell 7"), first({programFiles + QStringLiteral("/PowerShell/7/pwsh.exe"), QStringLiteral("pwsh")}));
    add(QStringLiteral("Windows PowerShell"), first({QStringLiteral("powershell")}));
#else
    QFile file(QStringLiteral("/etc/shells"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        for (const QByteArray &raw : file.readAll().split('\n')) {
            const QString path = QString::fromLocal8Bit(raw.trimmed());
            if (path.startsWith(QLatin1Char('/')) && QFileInfo(path).isExecutable())
                add(path, path);
        }
#endif
    return list;
}

QString defaultShell()
{
    QString shell = Settings::instance()->value(Settings::TermShell).toString().trimmed();
    if (!shell.isEmpty())
        return shell;
#ifdef Q_OS_WIN
    const QList<Shell> installed = shells();
    return installed.isEmpty() ? QStringLiteral("powershell.exe") : installed.first().program;
#else
    shell = qEnvironmentVariable("SHELL");
    if (shell.isEmpty())
        if (const passwd *pw = getpwuid(getuid()); pw && pw->pw_shell)
            shell = QString::fromLocal8Bit(pw->pw_shell);
    return shell.isEmpty() ? QStringLiteral("/bin/zsh") : shell;
#endif
}

static bool isPowerShellApp(const QString &id)
{
    return id == QLatin1String("powershell") || id == QLatin1String("pwsh") || id == QLatin1String("pwsh-preview");
}

QList<App> available()
{
    QList<App> apps;
    auto add = [&](const char *id, const QString &name, const QString &executable) {
        QString path = QStandardPaths::findExecutable(executable);
#ifdef Q_OS_MACOS
        if (path.isEmpty()) {
            const QString bundle = name == QLatin1String("kitty") ? QStringLiteral("kitty") : name;
            for (const QString &root : {QStringLiteral("/Applications"), QDir::homePath() + QStringLiteral("/Applications")}) {
                const QString candidate = root + QLatin1Char('/') + bundle + QStringLiteral(".app/Contents/MacOS/") + executable;
                if (QFileInfo(candidate).isExecutable()) {
                    path = candidate;
                    break;
                }
            }
        }
#endif
        if (!path.isEmpty())
            apps.append({QString::fromLatin1(id), name, path});
    };
#ifdef Q_OS_WIN
    add("wt", QStringLiteral("Windows Terminal"), QStringLiteral("wt.exe"));
    for (const Shell &shell : shells()) // newest first
        apps.append({shell.name == QLatin1String("Windows PowerShell") ? QStringLiteral("powershell")
                         : shell.name == QLatin1String("PowerShell 7") ? QStringLiteral("pwsh")
                                                                        : QStringLiteral("pwsh-preview"),
                     shell.name, shell.program});
#elif defined(Q_OS_MACOS)
    if (QDir(QStringLiteral("/System/Applications/Utilities/Terminal.app")).exists())
        apps.append({QStringLiteral("terminal"), QStringLiteral("Terminal"), QStringLiteral("/System/Applications/Utilities/Terminal.app")});
#else
    add("gnome-terminal", QStringLiteral("GNOME Terminal"), QStringLiteral("gnome-terminal"));
    add("konsole", QStringLiteral("Konsole"), QStringLiteral("konsole"));
    add("xfce4-terminal", QStringLiteral("Xfce Terminal"), QStringLiteral("xfce4-terminal"));
    add("xterm", QStringLiteral("XTerm"), QStringLiteral("xterm"));
#endif
    add("wezterm", QStringLiteral("WezTerm"), QStringLiteral("wezterm"));
    add("alacritty", QStringLiteral("Alacritty"), QStringLiteral("alacritty"));
#ifndef Q_OS_WIN
    add("kitty", QStringLiteral("kitty"), QStringLiteral("kitty"));
#endif
    return apps;
}

#ifndef Q_OS_WIN
static QString quoted(QString text)
{
    return QLatin1Char('\'') + text.replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
}
#endif

Command command(const App &app, const QString &directory, const QString &line)
{
    // Without a line to run, an external terminal starts its own default shell / profile.
    const bool ownShell = line.trimmed().isEmpty() && !isPowerShellApp(app.id) && app.id != QLatin1String("terminal");
    QString shell = isPowerShellApp(app.id) ? app.program : defaultShell();
    QStringList args;
#ifdef Q_OS_WIN
    args << QStringLiteral("-NoLogo") << QStringLiteral("-NoExit");
    if (!line.trimmed().isEmpty()) {
        QByteArray utf16;
        for (const QChar c : line) {
            utf16.append(char(c.unicode() & 255));
            utf16.append(char(c.unicode() >> 8));
        }
        args << QStringLiteral("-EncodedCommand") << QString::fromLatin1(utf16.toBase64());
    }
#else
    args << QStringLiteral("-l");
    if (!line.trimmed().isEmpty())
        args << QStringLiteral("-i") << QStringLiteral("-c")
             << (line + QStringLiteral("\nexec ") + quoted(shell) + QStringLiteral(" -l"));
#endif
    Command result{app.program, {}, directory};
    if (isPowerShellApp(app.id)) {
        result.arguments = args;
    } else if (app.id == QLatin1String("wt")) {
        result.arguments = {QStringLiteral("-w"), QStringLiteral("new"), QStringLiteral("new-tab"),
                            QStringLiteral("-d"), QDir::toNativeSeparators(directory)};
        if (!ownShell)
            result.arguments << shell << args;
    } else if (app.id == QLatin1String("wezterm")) {
        result.arguments = {QStringLiteral("start"), QStringLiteral("--cwd"), directory};
        if (!ownShell)
            result.arguments << QStringLiteral("--") << shell << args;
    } else {
        if (app.id == QLatin1String("gnome-terminal") || app.id == QLatin1String("xfce4-terminal")
            || app.id == QLatin1String("alacritty"))
            result.arguments << QStringLiteral("--working-directory") << directory;
        else if (app.id == QLatin1String("konsole"))
            result.arguments << QStringLiteral("--workdir") << directory;
        else if (app.id == QLatin1String("kitty"))
            result.arguments << QStringLiteral("--directory") << directory;
        const QString separator = app.id == QLatin1String("gnome-terminal") ? QStringLiteral("--")
            : app.id == QLatin1String("xfce4-terminal") ? QStringLiteral("--execute") : QStringLiteral("-e");
        if (ownShell)
            return result;
        if (app.id != QLatin1String("kitty"))
            result.arguments << separator;
        result.arguments << shell;
        result.arguments << args;
    }
    return result;
}

bool launch(const QString &directory, const QString &line)
{
    const QString id = Settings::instance()->value(Settings::TermApp).toString();
    for (const App &app : available()) {
        if (app.id != id)
            continue;
        Command cmd = command(app, directory, line);
#ifdef Q_OS_MACOS
        if (id == QLatin1String("terminal")) {
            // Terminal opens executable .command files. Use argv and a quoted script, without
            // AppleScript or interpolating paths into the user's command. Delete on execution.
            QTemporaryFile script(QDir::tempPath() + QStringLiteral("/gifiles-terminal-XXXXXX.command"));
            if (!script.open())
                return false;
            const QString shell = defaultShell();
            App direct{QStringLiteral("terminal"), {}, {}}; // "-e" shell args…
            const QStringList shellArgs = command(direct, directory, line).arguments.mid(2);
            QString text = QStringLiteral("#!/bin/sh\nrm -f -- ") + quoted(script.fileName())
                + QStringLiteral("\ncd -- ") + quoted(directory) + QStringLiteral(" || exit\nexec ") + quoted(shell);
            for (const QString &arg : shellArgs)
                text += QLatin1Char(' ') + quoted(arg);
            const QByteArray bytes = (text + QLatin1Char('\n')).toUtf8();
            if (script.write(bytes) != bytes.size() || !script.flush()
                || !script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
                return false;
            cmd = {QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), app.program, script.fileName()}, directory};
            script.setAutoRemove(false);
            script.close();
            if (QProcess::startDetached(cmd.program, cmd.arguments, cmd.directory))
                return true;
            QFile::remove(script.fileName());
            return false;
        }
#endif
        QProcess process;
        process.setProgram(cmd.program);
        process.setArguments(cmd.arguments);
        process.setWorkingDirectory(cmd.directory);
#ifdef Q_OS_WIN
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
            a->flags |= CREATE_NEW_CONSOLE;
            a->flags &= ~DWORD(CREATE_NO_WINDOW);
            a->startupInfo->dwFlags &= ~DWORD(STARTF_USESTDHANDLES);
        });
#endif
        return process.startDetached();
    }
    return false;
}
}
