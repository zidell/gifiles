#pragma once

#include <QCache>
#include <QDateTime>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QThreadPool>

// Asynchronous, size-bounded thumbnail cache shared by all windows.
class Thumbnails : public QObject {
    Q_OBJECT
public:
    static Thumbnails *instance();
    static bool canThumbnail(const QString &path);

    // Returns the thumbnail if cached; otherwise schedules generation and returns a null pixmap.
    QPixmap get(const QString &path, const QDateTime &mtime);

    static constexpr int kSize = 256;

signals:
    void ready(const QString &path);

private:
    explicit Thumbnails(QObject *parent = nullptr);
    QString key(const QString &path, const QDateTime &mtime) const;

    QCache<QString, QPixmap> m_cache;
    QSet<QString> m_pending;
    QSet<QString> m_failed;
    QThreadPool m_pool;
};
