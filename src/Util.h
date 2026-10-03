#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QString>
#include <QStringList>

// Translation context for all user-facing text: Gifiles::tr("한국어 원문"). Korean is the source
// language; i18n/gifiles_{en,ja,zh_CN}.ts hold the translations (scripts/i18n.sh, docs/history.md).
struct Gifiles {
    Q_DECLARE_TR_FUNCTIONS(Gifiles)
};

namespace util {

// Finder-style decimal sizes: "12 KB", "1.2 MB".
QString humanSize(qint64 bytes);
// "오늘 15:04", "어제 15:04", "9월 30일 15:04", "2025. 9. 30."
QString humanDate(const QDateTime &t);
// Korean kind label, e.g. "PNG 이미지", "폴더".
QString kindOf(const QFileInfo &fi);

// Split "a.tar.gz" -> ("a.tar", ".gz"); dotfiles have no extension.
void splitExt(const QString &name, QString *base, QString *ext);
// Free name in dir: "name" or Finder-style "name 복사본", "name 복사본 2"... (forceCopy: never the plain name)
QString uniqueCopyName(const QString &dir, const QString &name, bool forceCopy);
// "name", "name 2", "name 3"...
QString uniquePlainName(const QString &dir, const QString &name);

bool isInside(const QString &child, const QString &parent);
bool exists(const QString &path); // does not follow symlinks
// Directories that should open with the system instead of being browsed (macOS bundles).
bool isPackage(const QFileInfo &fi);
bool isHexColor(const QString &s);                // "#RRGGBB"
QStringList extensionList(const QString &text);    // "ZIP, .7z tar" -> {"zip", "7z", "tar"}
QString validateName(const QString &name); // empty if OK, otherwise an error message
QString displayName(const QString &path);  // last path component, or the volume/root label
QString nativeShortcutText(const QString &portable);

// Showing an item in the system file manager (Finder, Explorer, the Linux desktop's own via the
// freedesktop FileManager1 D-Bus interface): a folder opens as itself, a file opens its folder
// with the file selected.
struct Command {
    QString program;
    QStringList args;
};
Command revealCommand(const QString &path);
QString revealActionText(); // "Finder에서 열기", "탐색기에서 열기", "파일 관리자에서 열기" (translated)
QString revealActionId();   // the same, untranslated: the menu action's id (Shortcuts, config.toml)
QString revealShowText();   // "Finder에서 보기", ... for a button that shows one file

} // namespace util
