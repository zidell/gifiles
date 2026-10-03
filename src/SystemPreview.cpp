#include "SystemPreview.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QSet>

bool SystemPreview::available()
{
#if defined(Q_OS_MACOS)
    return QGuiApplication::platformName() == QLatin1String("cocoa");
#elif defined(Q_OS_WIN)
    return QGuiApplication::platformName() == QLatin1String("windows");
#else
    return false;
#endif
}

bool SystemPreview::isOfficeDocument(const QString &path)
{
    static const QSet<QString> suffixes = {
        // Microsoft Office
        QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("docm"), QStringLiteral("dot"), QStringLiteral("dotx"),
        QStringLiteral("xls"), QStringLiteral("xlsx"), QStringLiteral("xlsm"), QStringLiteral("xlsb"), QStringLiteral("xlt"),
        QStringLiteral("xltx"), QStringLiteral("ppt"), QStringLiteral("pptx"), QStringLiteral("pptm"), QStringLiteral("pps"),
        QStringLiteral("ppsx"), QStringLiteral("pot"), QStringLiteral("potx"),
        // OpenDocument, Apple iWork, RTF, Hangul
        QStringLiteral("odt"), QStringLiteral("ods"), QStringLiteral("odp"), QStringLiteral("pages"), QStringLiteral("numbers"),
        QStringLiteral("key"), QStringLiteral("rtf"), QStringLiteral("rtfd"), QStringLiteral("hwp"), QStringLiteral("hwpx"),
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
bool SystemPreview::canShow(const QString &)
{
    return false;
}

SystemPreview *SystemPreview::create(QWidget *)
{
    return nullptr;
}
#endif
