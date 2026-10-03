#include "Thumbnails.h"

#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPointer>
#include <QSet>
#ifdef HAVE_QTPDF
#include <QPdfDocument>
#endif

namespace {

const QSet<QString> &imageSuffixes()
{
    static const QSet<QString> s = [] {
        QSet<QString> r;
        for (const QByteArray &f : QImageReader::supportedImageFormats())
            r.insert(QString::fromLatin1(f).toLower());
        r.remove(QStringLiteral("pdf")); // handled by QtPdf when available
        r.remove(QStringLiteral("svgz"));
        return r;
    }();
    return s;
}

QImage render(const QString &path, int size)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
#ifdef HAVE_QTPDF
    if (suffix == QLatin1String("pdf")) {
        QPdfDocument doc;
        if (doc.load(path) != QPdfDocument::Error::None || doc.pageCount() < 1)
            return {};
        QSizeF pt = doc.pagePointSize(0);
        QSize px = pt.toSize().scaled(size, size, Qt::KeepAspectRatio);
        QImage img = doc.render(0, px);
        if (!img.isNull()) { // paper is white even if the PDF has no background
            QImage out(img.size(), QImage::Format_ARGB32_Premultiplied);
            out.fill(Qt::white);
            QPainter(&out).drawImage(0, 0, img);
            return out;
        }
        return img;
    }
#endif
    QImageReader r(path);
    r.setAutoTransform(true);
    QSize s = r.size();
    if (s.isValid() && (s.width() > size || s.height() > size))
        r.setScaledSize(s.scaled(size, size, Qt::KeepAspectRatio));
    QImage img = r.read();
    if (img.isNull())
        return {};
    if (img.width() > size || img.height() > size)
        img = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

} // namespace

Thumbnails *Thumbnails::instance()
{
    static Thumbnails *t = new Thumbnails;
    return t;
}

Thumbnails::Thumbnails(QObject *parent) : QObject(parent)
{
    m_cache.setMaxCost(96 * 1024); // KiB
    m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount() / 2, 4));
}

bool Thumbnails::canThumbnail(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
#ifdef HAVE_QTPDF
    if (suffix == QLatin1String("pdf"))
        return true;
#endif
    return imageSuffixes().contains(suffix);
}

QString Thumbnails::key(const QString &path, const QDateTime &mtime) const
{
    return path + QLatin1Char('|') + QString::number(mtime.toMSecsSinceEpoch());
}

QPixmap Thumbnails::get(const QString &path, const QDateTime &mtime)
{
    const QString k = key(path, mtime);
    if (QPixmap *p = m_cache.object(k))
        return *p;
    if (m_pending.contains(k) || m_failed.contains(k))
        return {};
    m_pending.insert(k);
    QPointer<Thumbnails> self(this);
    m_pool.start([self, path, k] {
        QImage img = render(path, kSize);
        QMetaObject::invokeMethod(
            Thumbnails::instance(),
            [self, path, k, img] {
                if (!self)
                    return;
                self->m_pending.remove(k);
                if (img.isNull()) {
                    self->m_failed.insert(k);
                    return;
                }
                auto *pm = new QPixmap(QPixmap::fromImage(img));
                self->m_cache.insert(k, pm, qMax<qsizetype>(1, img.sizeInBytes() / 1024));
                emit self->ready(path);
            },
            Qt::QueuedConnection);
    });
    return {};
}
