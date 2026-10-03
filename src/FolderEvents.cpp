#include "FolderEvents.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QPointer>

#ifdef Q_OS_MACOS
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <CoreServices/CoreServices.h>
#include <sys/stat.h>
#endif

namespace FolderEvents {

#ifdef Q_OS_MACOS

bool available() { return true; }

quint64 currentId() { return FSEventsGetCurrentEventId(); }

QString journalId(const QStringList &roots)
{
    QStringList ids;
    QStringList paths = roots;
    if (roots.contains(QStringLiteral("/")))
        paths << QStringLiteral("/System/Volumes/Data"); // the user's files live on the data volume
    for (const QString &p : paths) {
        struct stat st;
        if (::stat(QFile::encodeName(p).constData(), &st) != 0)
            return {};
        CFUUIDRef uuid = FSEventsCopyUUIDForDevice(st.st_dev);
        if (!uuid)
            return {};
        CFStringRef s = CFUUIDCreateString(nullptr, uuid);
        ids << QString::fromCFString(s);
        CFRelease(s);
        CFRelease(uuid);
    }
    ids.removeDuplicates();
    return ids.join(QLatin1Char(','));
}

struct Stream::Private {
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
    std::unique_ptr<QObject> context = std::make_unique<QObject>(); // queued calls die with it
    std::function<void(const Batch &)> callback;
    QList<QPair<QString, QString>> prefixes; // real path -> the path as written under the roots

    QString toIndexPath(const char *raw) const
    {
        QString p = QString::fromUtf8(raw).normalized(QString::NormalizationForm_C);
        while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
            p.chop(1);
        for (const auto &[real, written] : prefixes)
            if (p == real || p.startsWith(real + QLatin1Char('/')))
                return written + p.mid(real.size());
        return p;
    }

    static void received(ConstFSEventStreamRef, void *info, size_t count, void *paths, const FSEventStreamEventFlags flags[],
                         const FSEventStreamEventId ids[])
    {
        auto *self = static_cast<Private *>(info);
        auto *list = static_cast<char **>(paths);
        Batch b;
        for (size_t i = 0; i < count; ++i) {
            const FSEventStreamEventFlags f = flags[i];
            b.lastId = std::max<quint64>(b.lastId, ids[i]);
            if (f & kFSEventStreamEventFlagHistoryDone) {
                b.historyDone = true;
                continue;
            }
            if (f & (kFSEventStreamEventFlagEventIdsWrapped | kFSEventStreamEventFlagRootChanged)) {
                b.reset = true;
                continue;
            }
            const QString p = self->toIndexPath(list[i]);
            if (f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagMount | kFSEventStreamEventFlagUnmount)) {
                b.deep << p;
                b.changed << QFileInfo(p).path();
            } else {
                b.changed << p;
            }
        }
        QMetaObject::invokeMethod(self->context.get(), [self, b] { self->callback(b); }, Qt::QueuedConnection);
    }
};

Stream::Stream(const QStringList &roots, quint64 since, std::function<void(const Batch &)> callback) : d(new Private)
{
    d->callback = std::move(callback);
    d->context->moveToThread(qApp->thread());
    QStringList watch;
    for (const QString &r : roots) {
        const QString clean = QDir::cleanPath(r);
        const QString real = QFileInfo(clean).canonicalFilePath(); // /var/folders -> /private/var/folders
        if (!real.isEmpty() && real != clean)
            d->prefixes << qMakePair(real == QLatin1String("/") ? QString() : real, clean == QLatin1String("/") ? QString() : clean);
        watch << (real.isEmpty() ? clean : real);
    }
    QList<CFStringRef> strings;
    for (const QString &w : watch)
        strings << w.toCFString();
    CFArrayRef array = CFArrayCreate(nullptr, reinterpret_cast<const void **>(strings.data()), strings.size(), &kCFTypeArrayCallBacks);
    for (CFStringRef s : strings)
        CFRelease(s);
    FSEventStreamContext ctx{0, d.get(), nullptr, nullptr, nullptr};
    d->stream = FSEventStreamCreate(nullptr, &Private::received, &ctx, array, since, 0.3, kFSEventStreamCreateFlagNoDefer);
    CFRelease(array);
    if (!d->stream)
        return;
    d->queue = dispatch_queue_create("dev.zidell.gifiles.folder-events", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(d->stream, d->queue);
    FSEventStreamStart(d->stream);
}

Stream::~Stream()
{
    if (d->stream) {
        FSEventStreamStop(d->stream);
        FSEventStreamInvalidate(d->stream);
        dispatch_sync_f(d->queue, nullptr, [](void *) {}); // a callback still running finishes first
        FSEventStreamRelease(d->stream);
        dispatch_release(d->queue);
    }
}

#else

bool available() { return false; }
quint64 currentId() { return 0; }
QString journalId(const QStringList &) { return {}; }

struct Stream::Private {};
Stream::Stream(const QStringList &, quint64, std::function<void(const Batch &)>) : d(new Private) {}
Stream::~Stream() = default;

#endif

} // namespace FolderEvents
