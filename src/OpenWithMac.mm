#include "OpenWith.h"

#include <QFileIconProvider>
#include <QFileInfo>
#include <QUrl>

#import <AppKit/AppKit.h>

namespace OpenWith {

QList<App> platformApps(const QString &file)
{
    QList<App> apps;
    @autoreleasepool {
        NSURL *url = QUrl::fromLocalFile(file).toNSURL();
        NSWorkspace *ws = NSWorkspace.sharedWorkspace;
        NSURL *def = [ws URLForApplicationToOpenURL:url];
        NSArray<NSURL *> *urls = [ws URLsForApplicationsToOpenURL:url];
        QStringList seenNames;
        auto add = [&](NSURL *appUrl, bool isDefault) {
            const QString path = QString::fromNSString(appUrl.path);
            NSString *display = [NSFileManager.defaultManager displayNameAtPath:appUrl.path];
            QString name = QString::fromNSString(display);
            if (name.endsWith(QLatin1String(".app")))
                name.chop(4);
            // Copies of the same app (caches, old versions) are listed once.
            if (seenNames.contains(name))
                return;
            seenNames << name;
            apps << App{name, path, QFileIconProvider().icon(QFileInfo(path)), isDefault};
        };
        if (def)
            add(def, true);
        // Sorted by name after the default, like Finder's list.
        // ... preferring the copy in /Applications when there are several.
        NSArray<NSURL *> *sorted = [urls sortedArrayUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
            const NSComparisonResult byName = [a.lastPathComponent localizedStandardCompare:b.lastPathComponent];
            if (byName != NSOrderedSame)
                return byName;
            const BOOL aa = [a.path hasPrefix:@"/Applications/"], ba = [b.path hasPrefix:@"/Applications/"];
            return aa == ba ? NSOrderedSame : (aa ? NSOrderedAscending : NSOrderedDescending);
        }];
        for (NSURL *u in sorted)
            add(u, false);
    }
    return apps;
}

bool platformOpen(const QStringList &files, const QString &appId)
{
    @autoreleasepool {
        NSMutableArray<NSURL *> *urls = [NSMutableArray array];
        for (const QString &f : files)
            [urls addObject:QUrl::fromLocalFile(f).toNSURL()];
        NSURL *app = [NSURL fileURLWithPath:appId.toNSString()];
        if (![NSFileManager.defaultManager fileExistsAtPath:app.path])
            return false;
        [NSWorkspace.sharedWorkspace openURLs:urls
                         withApplicationAtURL:app
                                configuration:[NSWorkspaceOpenConfiguration configuration]
                            completionHandler:nil];
        return true;
    }
}

} // namespace OpenWith
