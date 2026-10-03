#pragma once

#include <QIcon>
#include <QList>
#include <QString>
#include <QStringList>

class QWidget;

// "다음으로 열기…": which apps can open a file, opening with a chosen one, and a per-extension
// "always open with this app" preference used whenever Gifiles opens files.
namespace OpenWith {

struct App {
    QString name;
    QString id; // macOS: .app path, Linux: desktop file id
    QIcon icon;
    bool isDefault = false;
};

QList<App> appsFor(const QString &file);
bool openWith(const QStringList &files, const QString &appId);
// Opens with the remembered app for the extension, or the system default.
void open(const QString &file);
// Whether the user picked "항상 이 앱으로 열기" for this file's extension.
bool hasRememberedApp(const QString &file);
// Windows shows the system "Open with" dialog (it has its own "always use" checkbox).
void showDialog(QWidget *parent, const QStringList &files);

// Platform hooks (OpenWithMac.mm / OpenWith.cpp)
QList<App> platformApps(const QString &file);
bool platformOpen(const QStringList &files, const QString &appId);

} // namespace OpenWith
