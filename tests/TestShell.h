#pragma once

#include <QFile>
#include <QTemporaryDir>

namespace gifiles_test {
// A login bash still reads the developer's ~/.profile and ~/.bashrc. In
// particular nvm can keep startup busy after the PTY first looks idle. Tests
// exercise line editing, not that personal setup, so exec a clean shell.
inline QString shell() {
#ifdef Q_OS_WIN
    return {};
#else
    static QTemporaryDir directory;
    static const QString path = [] {
        const QString name = directory.filePath(QStringLiteral("shell"));
        QFile file(name);
        if (!file.open(QIODevice::WriteOnly)
            || file.write("#!/bin/sh\nexec /bin/bash --noprofile --norc -i\n") < 0)
            return QString();
        file.close();
        if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
            return QString();
        return name;
    }();
    return path;
#endif
}
} // namespace gifiles_test
