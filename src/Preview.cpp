#include "Preview.h"
#include "MacWindow.h"
#include "Settings.h"
#include "Shortcuts.h"
#include "SystemPreview.h"
#include "Util.h"

#include <QAudio>
#include <QAudioOutput>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QApplication>
#include <QLabel>
#include <QLocale>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QVideoFrame>
#include <QVideoSink>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStringDecoder>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QtConcurrent>
#ifdef HAVE_QTPDF
#include <QPdfDocument>
#include <QPdfView>
#endif

namespace {

constexpr qint64 kMaxTextBytes = 512 * 1024;
constexpr qint64 kSeekStepMs = 5000; // ← / → in Quick Look while sound or video plays

// Documents (prose) are shown wrapped in the UI font, other text (code, config) in the fixed-width
// font; each with its own size from Settings → 미리보기.
bool isDocument(const QString &path)
{
    const QFileInfo fi(path);
    const QString suffix = fi.suffix().toLower();
    static const QStringList doc = {QStringLiteral("txt"), QStringLiteral("text"), QStringLiteral("md"),
                                    QStringLiteral("markdown"), QStringLiteral("rst")};
    static const QStringList code = {QStringLiteral("makefile"), QStringLiteral("dockerfile"), QStringLiteral("gemfile"),
                                     QStringLiteral("rakefile"), QStringLiteral("procfile"), QStringLiteral("brewfile"),
                                     QStringLiteral("justfile"), QStringLiteral("vagrantfile")};
    if (suffix.isEmpty())
        return !fi.fileName().startsWith(QLatin1Char('.')) && !code.contains(fi.fileName().toLower());
    return doc.contains(suffix);
}

// Quick Look scales the text with its window (+ / −, preview.quick_look_doc_scale).
QFont textFont(bool document, bool quickLook)
{
    QFont f = document ? QApplication::font() : QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const int pt = Settings::instance()->value(document ? Settings::PreviewDocFontSize : Settings::PreviewTextFontSize).toInt();
    f.setPointSizeF(quickLook ? pt * Settings::instance()->value(Settings::QuickLookDocScale).toInt() / 100.0 : pt);
    return f;
}

// Quick Look's window for content without a size of its own, at 100% (preview.quick_look_doc_scale).
QSize docBaseSize(bool audio) { return audio ? QSize(520, 260) : QSize(900, 680); }

QString formatTime(qint64 ms)
{
    const qint64 s = ms / 1000;
    return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
                     : QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

bool looksBinary(const QByteArray &b)
{
    const qsizetype n = qMin<qsizetype>(b.size(), 8192);
    if (b.left(n).contains('\0'))
        return true;
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString s = dec.decode(b.left(n));
    int bad = s.count(QChar::ReplacementCharacter);
    return bad * 20 > n;
}

struct LoadedImage {
    QImage image;
    QSize pixels; // full size of the file, after EXIF rotation
};

// Decodes an image, shrinking it to fit maxSide on each side.
LoadedImage loadImage(const QString &path, int maxSide)
{
    QImageReader r(path);
    r.setAutoTransform(true);
    QSize s = r.size();
    if (s.isValid() && (s.width() > maxSide || s.height() > maxSide))
        r.setScaledSize(s.scaled(maxSide, maxSide, Qt::KeepAspectRatio));
    const bool rotated = r.transformation() & QImageIOHandler::TransformationRotate90;
    LoadedImage out;
    out.image = r.read();
    if (s.isValid() && rotated)
        s.transpose();
    out.pixels = s.isValid() ? s : out.image.size();
    return out;
}

// Resolution stored in a PNG (pHYs) or JPEG (JFIF) header, read without decoding the image;
// 0 when there is none or for other formats. Matches what Qt's decoders put in dotsPerMeter.
qreal headerDpi(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray d = f.read(64 * 1024);
    auto u16 = [&](qsizetype i) { return (uchar(d[i]) << 8) | uchar(d[i + 1]); };
    auto u32 = [&](qsizetype i) { return (quint32(u16(i)) << 16) | quint32(u16(i + 2)); };
    if (d.startsWith("\x89PNG\r\n\x1a\n")) {
        for (qsizetype i = 8; i + 8 <= d.size();) {
            const quint32 len = u32(i);
            const QByteArray type = d.mid(i + 4, 4);
            if (type == "IDAT" || type == "IEND")
                break;
            if (type == "pHYs" && len >= 9 && i + 17 <= d.size())
                return uchar(d[i + 16]) == 1 ? u32(i + 8) * 0.0254 : 0; // unit 1: per metre
            i += 12 + qsizetype(len);
        }
    } else if (d.startsWith("\xff\xd8")) {
        for (qsizetype i = 2; i + 4 <= d.size() && uchar(d[i]) == 0xff;) {
            const int marker = uchar(d[i + 1]);
            if (marker == 0xda || marker == 0xd9) // start of scan / end of image
                break;
            const int len = u16(i + 2);
            if (marker == 0xe0 && i + 16 <= d.size() && d.mid(i + 4, 5) == QByteArray("JFIF\0", 5)) {
                const int unit = uchar(d[i + 11]), x = u16(i + 12);
                return unit == 1 ? x : unit == 2 ? x * 2.54 : 0;
            }
            i += 2 + len;
        }
    }
    return 0;
}

// Size in points the way Finder shows it: one pixel per point, except Retina (@2x / 144 dpi)
// images, which are shown at half their pixel size.
QSize pointSize(const QString &path, const QSize &pixels, qreal dpi)
{
    const bool retina = QFileInfo(path).completeBaseName().endsWith(QLatin1String("@2x")) || (dpi > 140 && dpi < 150);
    return retina ? pixels / 2 : pixels;
}

// Magnifier with + (show 100%) or − (fit), drawn so it reads on light and dark content.
QCursor magnifierCursor(bool zoomIn)
{
    auto make = [](bool plus) {
        const qreal dpr = qApp->devicePixelRatio();
        QPixmap pm(QSize(32, 32) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c(12, 12);
        auto stroke = [&](const QColor &color, qreal w, qreal handleW) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(color, handleW, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(17.6, 17.6), QPointF(25, 25));
            p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap));
            p.drawEllipse(c, 7.5, 7.5);
            p.drawLine(QPointF(8.8, 12), QPointF(15.2, 12));
            if (plus)
                p.drawLine(QPointF(12, 8.8), QPointF(12, 15.2));
        };
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 210));
        p.drawEllipse(c, 7.5, 7.5);
        stroke(Qt::white, 4, 5.5); // halo
        stroke(Qt::black, 1.8, 3.2);
        return QCursor(pm, 12, 12);
    };
    static const QCursor in = make(true), out = make(false);
    return zoomIn ? in : out;
}

} // namespace

// ---------------------------------------------------------------------------

ZoomArea::ZoomArea(QWidget *content, QWidget *parent) : QScrollArea(parent), m_content(content)
{
    setFrameShape(QFrame::NoFrame);
    setAlignment(Qt::AlignCenter);
    setWidgetResizable(false);
    setFocusPolicy(Qt::NoFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setAutoFillBackground(false);
    setWidget(content);
    content->setAutoFillBackground(false);
    viewport()->installEventFilter(this);
    content->installEventFilter(this);
}

void ZoomArea::setNaturalSize(const QSize &points)
{
    m_natural = points;
    m_fit = true;
    m_scale = 1.0;
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    relayout();
}

qreal ZoomArea::fitScale() const
{
    if (!m_natural.isValid() || m_natural.isEmpty())
        return 1.0;
    const QSize avail = maximumViewportSize();
    const qreal s = qMin(qreal(avail.width()) / m_natural.width(), qreal(avail.height()) / m_natural.height());
    return qMin(s, m_fitLimit);
}

qreal ZoomArea::scale() const
{
    return m_fit ? fitScale() : m_scale;
}

qreal ZoomArea::clickTarget() const
{
    return isActualSize() ? 0.0 : 1.0;
}

bool ZoomArea::isZoomable() const
{
    if (!m_natural.isValid() || m_natural.isEmpty())
        return false;
    const qreal t = clickTarget();
    return qAbs((t > 0 ? t : fitScale()) - scale()) > 0.01;
}

void ZoomArea::setActualSize(bool on, const QPoint &anchor)
{
    if (on)
        apply(false, 1.0, anchor);
    else
        apply(true, 1.0, anchor);
}

void ZoomArea::setFitLimit(qreal maxScale)
{
    m_fitLimit = maxScale;
    relayout();
}

void ZoomArea::apply(bool fit, qreal newScale, const QPoint &anchor)
{
    const QPoint at = anchor.isNull() ? viewport()->rect().center() : anchor;
    const QPoint c = m_content->mapFrom(viewport(), at);
    const QPointF frac(qBound(0.0, qreal(c.x()) / qMax(1, m_content->width()), 1.0),
                       qBound(0.0, qreal(c.y()) / qMax(1, m_content->height()), 1.0));
    m_fit = fit;
    m_scale = newScale;
    const auto policy = fit ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded;
    setHorizontalScrollBarPolicy(policy);
    setVerticalScrollBarPolicy(policy);
    relayout();
    // Keep the spot under the cursor (or the centre) in place.
    horizontalScrollBar()->setValue(qRound(frac.x() * m_content->width()) - at.x());
    verticalScrollBar()->setValue(qRound(frac.y() * m_content->height()) - at.y());
    emit scaleChanged(scale());
}

void ZoomArea::relayout()
{
    if (!m_natural.isValid() || m_natural.isEmpty()) {
        m_content->resize(maximumViewportSize());
    } else {
        const qreal s = scale();
        m_content->resize(QSize(qRound(m_natural.width() * s), qRound(m_natural.height() * s)).expandedTo(QSize(1, 1)));
    }
    updateCursor();
}

bool ZoomArea::canPan() const
{
    return horizontalScrollBar()->maximum() > 0 || verticalScrollBar()->maximum() > 0;
}

void ZoomArea::updateCursor()
{
    if (m_panned)
        return;
    if (isZoomable()) {
        const qreal t = clickTarget();
        const QCursor c = magnifierCursor((t > 0 ? t : fitScale()) > scale());
        viewport()->setCursor(c);
        m_content->setCursor(c);
    } else {
        viewport()->unsetCursor();
        m_content->unsetCursor();
    }
}

void ZoomArea::resizeEvent(QResizeEvent *e)
{
    QScrollArea::resizeEvent(e);
    relayout();
}

// A click toggles fit / 100%; at 100% dragging pans.
bool ZoomArea::eventFilter(QObject *obj, QEvent *ev)
{
    switch (ev->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (me->button() != Qt::LeftButton)
            break;
        m_pressed = true;
        m_panned = false;
        m_pressPos = me->globalPosition().toPoint();
        m_scrollAtPress = QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
        return true;
    }
    case QEvent::MouseMove: {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (!m_pressed || !canPan())
            break;
        const QPoint d = me->globalPosition().toPoint() - m_pressPos;
        if (!m_panned && d.manhattanLength() > QApplication::startDragDistance()) {
            m_panned = true;
            viewport()->setCursor(Qt::ClosedHandCursor);
            m_content->setCursor(Qt::ClosedHandCursor);
        }
        if (m_panned) {
            horizontalScrollBar()->setValue(m_scrollAtPress.x() - d.x());
            verticalScrollBar()->setValue(m_scrollAtPress.y() - d.y());
        }
        return true;
    }
    case QEvent::MouseButtonRelease: {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (me->button() != Qt::LeftButton || !m_pressed)
            break;
        m_pressed = false;
        const bool panned = m_panned;
        m_panned = false;
        if (!panned && isZoomable())
            setActualSize(!isActualSize(), viewport()->mapFromGlobal(me->globalPosition().toPoint()));
        updateCursor();
        return true;
    }
    default:
        break;
    }
    return QScrollArea::eventFilter(obj, ev);
}

// ---------------------------------------------------------------------------

PreviewWidget::PreviewWidget(Style kind, QWidget *parent) : QWidget(parent), m_style(kind)
{
    const bool pane = kind == Pane;
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(pane ? 8 : 0, pane ? 8 : 0, pane ? 8 : 0, 4);
    root->setSpacing(6);

    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack, 1);

    // Image
    m_image = new QLabel;
    m_image->setScaledContents(true); // ZoomArea sizes the label to the image's aspect ratio
    m_imageArea = new ZoomArea(m_image, this);
    m_stack->addWidget(m_imageArea);
    connect(m_imageArea, &ZoomArea::scaleChanged, this, [this] {
        // Decode the full image once it is shown larger than the screen-sized copy.
        const QSize shown = m_image->size() * devicePixelRatioF();
        if (shown.width() > m_imageLoaded.width() || shown.height() > m_imageLoaded.height())
            loadFullImage();
    });

    // Text
    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setFont(textFont(false, m_style == QuickLook));
    m_text->setFrameShape(QFrame::NoFrame);
    m_stack->addWidget(m_text);
    connect(Settings::instance(), &Settings::changed, m_text, [this](const QString &key) {
        if (key == QLatin1String(Settings::PreviewTextFontSize) || key == QLatin1String(Settings::PreviewDocFontSize) ||
            key == QLatin1String(Settings::QuickLookDocScale) || key.isEmpty())
            m_text->setFont(textFont(isDocument(m_path), m_style == QuickLook));
    });


    // Info card
    m_infoPage = new QWidget(this);
    auto *il = new QVBoxLayout(m_infoPage);
    il->addStretch();
    m_bigIcon = new QLabel(m_infoPage);
    m_bigIcon->setAlignment(Qt::AlignCenter);
    m_infoNote = new QLabel(m_infoPage);
    m_infoNote->setAlignment(Qt::AlignCenter);
    m_infoNote->setWordWrap(true);
    il->addWidget(m_bigIcon);
    il->addWidget(m_infoNote);
    il->addStretch();
    m_stack->addWidget(m_infoPage);

    m_title = new QLabel(this);
    m_title->setAlignment(pane ? Qt::AlignLeft : Qt::AlignCenter);
    m_title->setWordWrap(true);
    QFont tf = m_title->font();
    tf.setBold(true);
    m_title->setFont(tf);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details = new QLabel(this);
    m_details->setAlignment(m_title->alignment());
    m_details->setWordWrap(true);
    m_details->setForegroundRole(QPalette::PlaceholderText);
    root->addWidget(m_title);
    root->addWidget(m_details);
    m_title->setVisible(pane);
}

// A slider whose groove jumps to the clicked point (Qt's own only pages towards it); the press
// then goes on to the handle that is now under the pointer, so a drag continues from there.
static void jumpOnClick(QSlider *slider, std::function<void(int)> moved)
{
    struct Jump : QObject {
        std::function<void(int)> moved;
        using QObject::QObject;
        bool eventFilter(QObject *obj, QEvent *ev) override
        {
            auto *s = static_cast<QSlider *>(obj);
            if (ev->type() != QEvent::MouseButtonPress || static_cast<QMouseEvent *>(ev)->button() != Qt::LeftButton ||
                s->maximum() <= s->minimum())
                return false;
            QStyleOptionSlider opt;
            opt.initFrom(s);
            opt.orientation = s->orientation();
            opt.minimum = s->minimum();
            opt.maximum = s->maximum();
            opt.sliderPosition = opt.sliderValue = s->value();
            const QRect handle = s->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, s);
            const QPoint pos = static_cast<QMouseEvent *>(ev)->position().toPoint();
            if (handle.contains(pos))
                return false;
            const QRect groove = s->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, s);
            const int span = groove.width() - handle.width();
            const int v = QStyle::sliderValueFromPosition(s->minimum(), s->maximum(), pos.x() - groove.x() - handle.width() / 2, span,
                                                          opt.upsideDown);
            s->setValue(v);
            if (moved)
                moved(v);
            return false;
        }
    };
    auto *j = new Jump(slider);
    j->moved = std::move(moved);
    slider->installEventFilter(j);
}

// The media player (FFmpeg backend, audio device) and PDF engine are created on first use:
// most previews never need them, and every tab owns a preview widget.
void PreviewWidget::ensureMedia()
{
    if (m_player)
        return;
    m_mediaPage = new QWidget(this);
    auto *ml = new QVBoxLayout(m_mediaPage);
    ml->setContentsMargins(0, 0, 0, 0);
    m_video = new QVideoWidget;
    m_videoArea = new ZoomArea(m_video, m_mediaPage);
    m_audioIcon = new QLabel(m_mediaPage);
    m_audioIcon->setAlignment(Qt::AlignCenter);
    ml->addWidget(m_videoArea, 1);
    ml->addWidget(m_audioIcon, 1);
    auto *controls = new QHBoxLayout;
    m_play = new QToolButton(m_mediaPage);
    m_play->setAutoRaise(true);
    m_seek = new QSlider(Qt::Horizontal, m_mediaPage);
    m_time = new QLabel(QStringLiteral("0:00"), m_mediaPage);
    // Volume, remembered (preview.volume); a perceptual (logarithmic) slider.
    auto *speaker = new QLabel(m_mediaPage);
    speaker->setPixmap(style()->standardIcon(QStyle::SP_MediaVolume).pixmap(16, 16));
    m_volume = new QSlider(Qt::Horizontal, m_mediaPage);
    m_volume->setObjectName(QStringLiteral("volume"));
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(80);
    m_volume->setValue(Settings::instance()->value(Settings::MediaVolume).toInt());
    if (m_style == QuickLook) // the window has no margins of its own: keep the ends off its edges
        controls->setContentsMargins(10, 2, 16, 2);
    controls->setSpacing(8);
    controls->addWidget(m_play);
    controls->addWidget(m_seek, 1);
    controls->addWidget(m_time);
    controls->addSpacing(6);
    controls->addWidget(speaker);
    controls->addWidget(m_volume);
    ml->addLayout(controls);
    m_stack->addWidget(m_mediaPage);

    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    auto applyVolume = [this](int v) {
        m_audio->setVolume(QAudio::convertVolume(v / 100.0, QAudio::LogarithmicVolumeScale, QAudio::LinearVolumeScale));
    };
    applyVolume(m_volume->value());
    connect(m_volume, &QSlider::valueChanged, this, [this, applyVolume](int v) {
        applyVolume(v);
        if (!m_volume->isSliderDown()) // clicks and keys; a drag is saved when released
            Settings::instance()->setValue(Settings::MediaVolume, v);
    });
    connect(m_volume, &QSlider::sliderReleased, this, [this] { Settings::instance()->setValue(Settings::MediaVolume, m_volume->value()); });
    connect(Settings::instance(), &Settings::changed, m_volume, [this](const QString &key) {
        if (key == QLatin1String(Settings::MediaVolume) || key.isEmpty())
            m_volume->setValue(Settings::instance()->value(Settings::MediaVolume).toInt());
    });
    m_player->setVideoOutput(m_video);
    connect(m_video->videoSink(), &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &f) {
        QSize s = f.size();
        if (f.rotation() == QtVideo::Rotation::Clockwise90 || f.rotation() == QtVideo::Rotation::Clockwise270)
            s.transpose();
        setVideoNaturalSize(s);
        if (m_seekTarget >= 0 && m_seekFloor < 0 && f.startTime() / 1000 >= m_seekTarget - 50)
            endResume(true); // the frame where it stopped is up
    });
    // The container's metadata usually gives the size before the first frame is decoded (and
    // while Quick Look is still hidden, waiting for it).
    connect(m_player, &QMediaPlayer::metaDataChanged, this, [this] {
        if (!m_videoArea->isVisibleTo(m_mediaPage))
            return;
        const QMediaMetaData md = m_player->metaData();
        QSize s = md.value(QMediaMetaData::Resolution).toSize();
        if (md.value(QMediaMetaData::Orientation).toInt() % 180 == 90)
            s.transpose();
        setVideoNaturalSize(s);
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus st) {
        if ((st == QMediaPlayer::LoadedMedia || st == QMediaPlayer::BufferedMedia) && m_pendingSeek) {
            const qint64 target = m_pendingSeek;
            m_pendingSeek = 0;
            beginResume(target);
        } else if (st == QMediaPlayer::EndOfMedia && m_player->source().toLocalFile() == m_resumePath) {
            m_resumePath.clear(); // played to the end: next time from the start
        }
    });
    connect(m_play, &QToolButton::clicked, this, &PreviewWidget::togglePlay);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState s) {
        m_play->setIcon(style()->standardIcon(s == QMediaPlayer::PlayingState ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 d) { m_seek->setRange(0, int(d)); });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 p) {
        if (!m_seek->isSliderDown())
            m_seek->setValue(int(p));
        if (m_seekFloor >= 0 && m_player->position() > m_seekFloor) { // playing: now the exact spot
            m_seekFloor = -1;
            m_player->setPosition(m_seekTarget);
            if (!m_videoArea->isVisibleTo(m_mediaPage))
                endResume(true); // sound: no frame to wait for
        }
        m_time->setText(formatTime(p) + QStringLiteral(" / ") + formatTime(m_player->duration()));
    });
    connect(m_seek, &QSlider::sliderMoved, m_player, [this](int v) {
        endResume(false);
        m_player->setPosition(v);
    });
    // A click anywhere on the bars jumps there (and can be dragged on from there).
    jumpOnClick(m_seek, [this](int v) {
        endResume(false);
        m_player->setPosition(v);
    });
    jumpOnClick(m_volume, {});
    m_play->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
}

void PreviewWidget::ensurePdf()
{
#ifdef HAVE_QTPDF
    if (m_pdfDoc)
        return;
    m_pdfDoc = new QPdfDocument(this);
    m_pdf = new QPdfView(this);
    m_pdf->setDocument(m_pdfDoc);
    m_pdf->setPageMode(QPdfView::PageMode::MultiPage);
    m_pdf->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    m_stack->addWidget(m_pdf);
#endif
}

PreviewWidget::~PreviewWidget()
{
    // QProcess destruction emits finished synchronously. Stop conversions before QWidget
    // destroys the preview children, and prevent completion handlers from touching them.
    for (QProcess *proc : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) {
        disconnect(proc, nullptr, this, nullptr);
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished();
        }
    }
}

void PreviewWidget::setPath(const QString &path)
{
    if (path == m_path)
        return;
    stopMedia();
    m_path = path;
    ++m_generation;
    if (m_movie) {
        m_movie->deleteLater();
        m_movie = nullptr;
    }
    m_image->clear();
    m_imagePixels = m_imageLoaded = m_resolution = QSize();
    m_loadingFull = false;
    m_sizePending = false;
    m_imageArea->setActualSize(false);
    m_imageArea->setNaturalSize(QSize());
    if (m_system)
        m_system->clear();
    updateDetails();

    QFileInfo fi(path);
    const QString suffix = fi.suffix().toLower();
    if (SystemPreview::isOfficeDocument(path) && showDocument(path)) {
        emit naturalSizeChanged(QSize());
        return;
    }
    const QString mname = path.isEmpty() || fi.isDir() ? QString() : QMimeDatabase().mimeTypeForFile(fi).name();
    if (mname.startsWith(QLatin1String("video/"))) {
        showMedia(path, true); // reports its size with the first frame
        return;
    }
#ifdef HAVE_QTPDF
    // Before images: Qt PDF also installs a "pdf" image plugin, which would show only the first page.
    if (suffix == QLatin1String("pdf") && !mname.isEmpty()) {
        showPdf(path);
        emit naturalSizeChanged(QSize());
        return;
    }
#endif
    if (!mname.isEmpty() && (QImageReader::supportedImageFormats().contains(suffix.toLatin1()) || mname.startsWith(QLatin1String("image/")))) {
        showImage(path); // reports its size once decoded
        return;
    }
    if (mname.isEmpty())
        showInfo();
    else if (mname.startsWith(QLatin1String("audio/")))
        showMedia(path, false);
    else if (!showText(path) && !showSystem(path))
        showInfo();
    emit naturalSizeChanged(QSize());
}

// Word, Excel, Pages, …: the system's preview (macOS Quick Look, a Windows preview handler);
// on Linux a PDF made by LibreOffice when it is installed.
bool PreviewWidget::showDocument(const QString &path)
{
    return showSystem(path) || convertWithOffice(path);
}

bool PreviewWidget::showSystem(const QString &path)
{
    if (!SystemPreview::canShow(path))
        return false;
    if (!m_system) {
        m_system = SystemPreview::create(this);
        if (!m_system)
            return false;
        m_stack->addWidget(m_system);
    }
    m_stack->setCurrentWidget(m_system); // shown first: a Windows handler draws into the window as it is
    if (m_system->show(path))
        return true;
    m_stack->setCurrentWidget(m_infoPage);
    return false;
}

bool PreviewWidget::convertWithOffice(const QString &path)
{
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN) && defined(HAVE_QTPDF)
    static const QString office = [] {
        const QString s = QStandardPaths::findExecutable(QStringLiteral("soffice"));
        return s.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("libreoffice")) : s;
    }();
    if (office.isEmpty())
        return false;
    // One PDF per file version, in the cache; folders unused for 30 days go once per run.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/office");
    static bool pruned = false;
    if (!pruned) {
        pruned = true;
        const QDateTime old = QDateTime::currentDateTime().addDays(-30);
        for (const QFileInfo &d : QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (d.fileName() != QLatin1String("profile") && d.lastModified() < old)
                QDir(d.absoluteFilePath()).removeRecursively();
    }
    const QFileInfo fi(path);
    const QByteArray id = (fi.absoluteFilePath() + QString::number(fi.lastModified().toMSecsSinceEpoch())).toUtf8();
    const QString outDir = base + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex());
    const QString pdf = outDir + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".pdf");
    if (QFileInfo::exists(pdf)) {
        showPdf(pdf);
        return true;
    }
    showInfo(Gifiles::tr("문서를 변환하는 중…"));
    auto *proc = new QProcess(this);
    const int generation = m_generation;
    auto done = [this, proc, generation, pdf] {
        proc->deleteLater();
        if (generation != m_generation)
            return;
        if (QFileInfo::exists(pdf))
            showPdf(pdf);
        else
            showInfo(Gifiles::tr("미리 볼 수 없는 문서입니다"));
    };
    connect(proc, &QProcess::finished, this, done);
    connect(proc, &QProcess::errorOccurred, this, [done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            done();
    });
    // Its own profile: a LibreOffice the user has open would otherwise take the job and ignore --headless.
    proc->start(office, {QStringLiteral("-env:UserInstallation=") + QUrl::fromLocalFile(base + QStringLiteral("/profile")).toString(),
                         QStringLiteral("--headless"), QStringLiteral("--convert-to"), QStringLiteral("pdf"), QStringLiteral("--outdir"),
                         outDir, fi.absoluteFilePath()});
    return true;
#else
    Q_UNUSED(path);
    return false;
#endif
}

void PreviewWidget::setFitLimit(qreal maxScale)
{
    m_imageArea->setFitLimit(maxScale);
    if (m_videoArea)
        m_videoArea->setFitLimit(maxScale);
}

QSize PreviewWidget::contentViewportSize() const
{
    if (m_stack->currentWidget() == m_imageArea)
        return m_imageArea->maximumViewportSize();
    if (m_mediaPage && m_stack->currentWidget() == m_mediaPage && m_videoArea->isVisible())
        return m_videoArea->maximumViewportSize();
    return {};
}

qreal PreviewWidget::contentScale() const
{
    if (m_stack->currentWidget() == m_imageArea)
        return m_imageArea->scale();
    if (m_mediaPage && m_stack->currentWidget() == m_mediaPage && m_videoArea->isVisibleTo(m_mediaPage))
        return m_videoArea->scale();
    return 0;
}

QSize PreviewWidget::chromeEstimate(int width) const
{
    // Everything around the viewport: the details line under the stack (Quick Look hides the
    // title), and for a video the player controls.
    const auto *root = static_cast<QBoxLayout *>(layout());
    const QMargins m = root->contentsMargins();
    const int w = width - m.left() - m.right();
    int h = m.top() + m.bottom() + root->spacing() + m_details->heightForWidth(w);
    if (m_style == Pane)
        h += root->spacing() + m_title->heightForWidth(w);
    if (m_mediaPage && m_stack->currentWidget() == m_mediaPage) {
        int controls = 0;
        for (QWidget *w : {static_cast<QWidget *>(m_play), static_cast<QWidget *>(m_seek), static_cast<QWidget *>(m_time)})
            controls = qMax(controls, w->sizeHint().height());
        h += m_mediaPage->layout()->spacing() + controls;
    }
    return {m.left() + m.right(), h};
}

void PreviewWidget::setVideoNaturalSize(const QSize &pixels)
{
    if (!pixels.isValid() || pixels == m_videoArea->naturalSize())
        return;
    m_videoArea->setNaturalSize(pixels);
    m_sizePending = false;
    m_resolution = pixels;
    updateDetails();
    emit naturalSizeChanged(pixels);
}

void PreviewWidget::updateDetails()
{
    if (m_path.isEmpty()) {
        m_title->clear();
        m_details->clear();
        return;
    }
    QFileInfo fi(m_path);
    m_title->setText(util::displayName(m_path));
    QStringList parts{util::kindOf(fi)};
    if (m_resolution.isValid())
        parts << QStringLiteral("%1 × %2").arg(m_resolution.width()).arg(m_resolution.height());
    if (fi.isDir()) {
        const int n = QDir(m_path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        parts << Gifiles::tr("%1개 항목").arg(n);
    } else {
        parts << util::humanSize(fi.size());
    }
    parts << QLocale().toString(fi.lastModified(), QLocale::ShortFormat);
    m_details->setText(parts.join(QStringLiteral(" · ")));
}

void PreviewWidget::showInfo(const QString &note)
{
    const int sz = m_style == QuickLook ? 256 : 128;
    if (!m_path.isEmpty())
        m_bigIcon->setPixmap(QFileIconProvider().icon(QFileInfo(m_path)).pixmap(sz, sz));
    else
        m_bigIcon->clear();
    m_infoNote->setText(note);
    m_stack->setCurrentWidget(m_infoPage);
}

void PreviewWidget::showImage(const QString &path)
{
    if (path.endsWith(QLatin1String(".gif"), Qt::CaseInsensitive)) {
        auto *mv = new QMovie(path, QByteArray(), this);
        if (mv->isValid() && mv->frameCount() != 1) {
            m_movie = mv;
            m_image->setMovie(mv);
            mv->start();
            const QImage first = mv->currentImage();
            const QSize pts = pointSize(path, first.size(), first.dotsPerMeterX() * 0.0254);
            m_imagePixels = m_imageLoaded = m_resolution = mv->currentImage().size();
            updateDetails();
            m_stack->setCurrentWidget(m_imageArea);
            m_imageArea->setNaturalSize(pts);
            emit naturalSizeChanged(pts);
            return;
        }
        delete mv;
    }
    // The size comes from the file header, so Quick Look gets its final size at once and the
    // decoded picture just fills it in; without one, the size waits for the decode.
    QImageReader header(path);
    header.setAutoTransform(true);
    QSize pixels = header.size();
    if (pixels.isValid() && header.transformation() & QImageIOHandler::TransformationRotate90)
        pixels.transpose();
    if (pixels.isValid()) {
        m_imagePixels = m_resolution = pixels;
        updateDetails();
        m_stack->setCurrentWidget(m_imageArea);
        const QSize pts = pointSize(path, pixels, headerDpi(path));
        m_imageArea->setNaturalSize(pts);
        emit naturalSizeChanged(pts);
    } else {
        m_sizePending = true;
        showInfo(Gifiles::tr("불러오는 중…"));
    }
    const int gen = m_generation;
    QPointer<PreviewWidget> self(this);
    auto *watcher = new QFutureWatcher<LoadedImage>(this);
    connect(watcher, &QFutureWatcher<LoadedImage>::finished, this, [self, watcher, gen, path] {
        watcher->deleteLater();
        if (!self || gen != self->m_generation)
            return;
        const LoadedImage li = watcher->result();
        if (li.image.isNull()) {
            self->m_sizePending = false;
            self->m_resolution = QSize();
            self->updateDetails();
            self->showInfo(Gifiles::tr("이미지를 열 수 없습니다"));
            emit self->naturalSizeChanged(QSize());
            return;
        }
        self->m_imagePixels = self->m_resolution = li.pixels;
        self->updateDetails();
        self->m_imageLoaded = li.image.size();
        self->m_image->setPixmap(QPixmap::fromImage(li.image));
        self->m_stack->setCurrentWidget(self->m_imageArea);
        // Only a format whose resolution the header check doesn't read can still change it here.
        const QSize pts = pointSize(path, li.pixels, li.image.dotsPerMeterX() * 0.0254);
        self->m_sizePending = false;
        if (pts != self->m_imageArea->naturalSize()) {
            self->m_imageArea->setNaturalSize(pts);
            emit self->naturalSizeChanged(pts);
        }
    });
    // A screen-sized decode is enough to fit; the full image is decoded when shown at 100%.
    watcher->setFuture(QtConcurrent::run([path] { return loadImage(path, 3000); }));
}

void PreviewWidget::loadFullImage()
{
    if (m_movie || m_loadingFull || m_imageLoaded == m_imagePixels || m_path.isEmpty())
        return;
    m_loadingFull = true;
    const int gen = m_generation;
    const QString path = m_path;
    QPointer<PreviewWidget> self(this);
    auto *watcher = new QFutureWatcher<LoadedImage>(this);
    connect(watcher, &QFutureWatcher<LoadedImage>::finished, this, [self, watcher, gen] {
        watcher->deleteLater();
        if (!self || gen != self->m_generation)
            return;
        const LoadedImage li = watcher->result();
        if (li.image.isNull())
            return;
        self->m_imageLoaded = li.image.size();
        self->m_image->setPixmap(QPixmap::fromImage(li.image));
    });
    watcher->setFuture(QtConcurrent::run([path] { return loadImage(path, 16384); }));
}

bool PreviewWidget::showText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = f.read(kMaxTextBytes);
    if (data.isEmpty()) {
        showInfo(Gifiles::tr("빈 파일"));
        return true;
    }
    if (looksBinary(data))
        return false;
    QString text = QString::fromUtf8(data);
    if (f.size() > kMaxTextBytes)
        text += Gifiles::tr("\n\n— 처음 %1만 표시 —").arg(util::humanSize(kMaxTextBytes));
    const bool prose = isDocument(path);
    m_text->setFont(textFont(prose, m_style == QuickLook));
    m_text->setLineWrapMode(prose ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    m_text->setPlainText(text);
    m_stack->setCurrentWidget(m_text);
    return true;
}

void PreviewWidget::showMedia(const QString &path, bool video)
{
    ensureMedia();
    m_videoArea->setActualSize(false);
    m_videoArea->setNaturalSize(QSize());
    m_videoArea->setVisible(video);
    m_audioIcon->setVisible(!video);
    if (!video)
        m_audioIcon->setPixmap(QFileIconProvider().icon(QFileInfo(path)).pixmap(128, 128));
    m_pendingSeek = path == m_resumePath ? m_resumePos : 0;
    m_player->setSource(QUrl::fromLocalFile(path));
    m_stack->setCurrentWidget(m_mediaPage);
    m_sizePending = video; // until the first frame
    if (m_style == QuickLook)
        m_player->play();
}

void PreviewWidget::showPdf(const QString &path)
{
#ifdef HAVE_QTPDF
    ensurePdf();
    m_pdfDoc->close();
    if (m_pdfDoc->load(path) != QPdfDocument::Error::None) {
        showInfo(Gifiles::tr("PDF를 열 수 없습니다"));
        return;
    }
    m_stack->setCurrentWidget(m_pdf);
#else
    Q_UNUSED(path);
    showInfo();
#endif
}

void PreviewWidget::stopMedia()
{
    if (!m_player || m_player->source().isEmpty())
        return;
    // Remember where it stopped; a file that never started leaves the earlier one remembered.
    const QString file = m_player->source().toLocalFile();
    const qint64 pos = m_pendingSeek ? m_pendingSeek : m_seekTarget >= 0 ? m_seekTarget : m_player->position();
    if (m_player->mediaStatus() == QMediaPlayer::EndOfMedia) {
        if (file == m_resumePath)
            m_resumePath.clear();
    } else if (pos > 0) {
        m_resumePath = file;
        m_resumePos = pos;
    }
    m_pendingSeek = 0;
    endResume(false);
    m_player->stop();
    m_player->setSource(QUrl());
    // The file has to load again when it is shown next (Quick Look reopened on the same video:
    // setPath would otherwise skip it and leave the player without a source).
    m_path.clear();
}

// Going on where it stopped. Qt's macOS player turns a position into the file's time scale, and
// that is whole seconds until playback has run (avfmediaplayer.mm: value = pos / 1000 *
// currentTime.timescale): a fresh file at 2.4 s would start at 2 s. So: seek to the whole
// second, play from there unseen and unheard, seek to the exact spot as soon as the position
// moves, and show it with its first frame there (paused again if it wasn't playing).
void PreviewWidget::chooseMediaBackend()
{
#ifdef Q_OS_MACOS
    // AVFoundation on macOS: Homebrew's Qt has only it, the resume below is written for it, and
    // Qt's own packages (the published app) would otherwise pick FFmpeg, where it never goes on.
    if (qEnvironmentVariableIsEmpty("QT_MEDIA_BACKEND"))
        qputenv("QT_MEDIA_BACKEND", "darwin");
#endif
}

void PreviewWidget::beginResume(qint64 target)
{
#ifdef Q_OS_MACOS
    const qint64 floor = target / 1000 * 1000;
    if (target - floor >= 40) {
        m_seekTarget = target;
        m_seekFloor = floor;
        m_resumePaused = m_player->playbackState() != QMediaPlayer::PlayingState;
        m_audio->setMuted(true);
        m_video->hide();
        m_player->setPosition(floor);
        if (m_resumePaused)
            m_player->play();
        QTimer::singleShot(1500, this, [this, gen = m_generation] {
            if (gen == m_generation)
                endResume(true); // never left hidden
        });
        return;
    }
#endif
    m_player->setPosition(target);
}

void PreviewWidget::endResume(bool restorePause)
{
    if (m_seekTarget < 0)
        return;
    const bool pause = restorePause && m_resumePaused;
    m_seekTarget = m_seekFloor = -1;
    m_audio->setMuted(false);
    m_video->show();
    if (pause)
        m_player->pause();
}

void PreviewWidget::forgetPlaybackOutside(const QString &folder)
{
    if (!m_resumePath.isEmpty() && QFileInfo(m_resumePath).absolutePath() != QFileInfo(folder).absoluteFilePath())
        m_resumePath.clear();
}

void PreviewWidget::togglePlay()
{
    if (!m_player)
        return;
    if (m_seekTarget >= 0) { // pressed while going to where it stopped: it is already playing
        const bool wasPaused = m_resumePaused;
        endResume(false);
        if (wasPaused)
            return;
    }
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else if (!m_player->source().isEmpty())
        m_player->play();
}

bool PreviewWidget::isPlayingMedia() const
{
    return m_mediaPage && m_stack->currentWidget() == m_mediaPage;
}

void PreviewWidget::seekBy(qint64 ms)
{
    if (!isPlayingMedia() || m_player->duration() <= 0)
        return;
    endResume(false);
    m_player->setPosition(qBound<qint64>(0, m_player->position() + ms, m_player->duration()));
}

bool PreviewWidget::isAudio() const
{
    return isPlayingMedia() && !m_videoArea->isVisibleTo(m_mediaPage);
}

void PreviewWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (m_player && m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
}

// ---------------------------------------------------------------------------

QuickLookWindow::QuickLookWindow(QWidget *parent) : QWidget(parent, Qt::Tool)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    m_preview = new PreviewWidget(PreviewWidget::QuickLook, this);
    l->addWidget(m_preview);
    resize(docBaseSize(false));
    connect(m_preview, &PreviewWidget::naturalSizeChanged, this, &QuickLookWindow::fitToContent);
    m_sizeSave = new QTimer(this);
    m_sizeSave->setSingleShot(true);
    m_sizeSave->setInterval(400);
    connect(m_sizeSave, &QTimer::timeout, this, &QuickLookWindow::rememberUserSize);
}

QSize QuickLookWindow::windowSizeFor(bool audio) const
{
    Settings *s = Settings::instance();
    const QSize stored(s->value(QString::fromLatin1(audio ? Settings::QuickLookAudioWidth : Settings::QuickLookDocWidth)).toInt(),
                       s->value(QString::fromLatin1(audio ? Settings::QuickLookAudioHeight : Settings::QuickLookDocHeight)).toInt());
    if (stored.width() > 0 && stored.height() > 0)
        return stored;
    if (audio)
        return docBaseSize(true);
    return (QSizeF(docBaseSize(false)) * (s->value(Settings::QuickLookDocScale).toInt() / 100.0)).toSize();
}

// The user dragged the window's edge: an image or video follows it (and its scale against the
// original is remembered); text, PDF and sound remember the window size, each kind its own.
void QuickLookWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (!isVisible() || m_showPending || size() == m_appliedSize)
        return;
    if (m_natural.isValid()) {
        // The picture grows and shrinks with the window, by as much as the window did.
        if (!m_sizeSave->isActive()) {
            m_dragFrom = m_appliedSize.isValid() ? m_appliedSize : e->oldSize();
            m_dragScale = m_preview->contentScale();
        }
        if (m_dragFrom.isValid() && !m_dragFrom.isEmpty() && m_dragScale > 0)
            m_preview->setFitLimit(m_dragScale * qMin(qreal(width()) / m_dragFrom.width(), qreal(height()) / m_dragFrom.height()));
    }
    m_sizeSave->start();
}

void QuickLookWindow::rememberUserSize()
{
    m_sizeSave->stop();
    if (size() == m_appliedSize)
        return;
    m_appliedSize = size();
    Settings *s = Settings::instance();
    if (m_natural.isValid()) {
        const qreal k = m_preview->contentScale();
        if (k <= 0)
            return;
        m_preview->setFitLimit(k);
        s->setValue(Settings::QuickLookScale, qBound(25, qRound(k * 100), 400));
    } else {
        const bool audio = m_preview->isAudio();
        s->setValue(QString::fromLatin1(audio ? Settings::QuickLookAudioWidth : Settings::QuickLookDocWidth), width());
        s->setValue(QString::fromLatin1(audio ? Settings::QuickLookAudioHeight : Settings::QuickLookDocHeight), height());
    }
}

// An image or video is shown at its own size times the remembered Quick Look scale (100% unless
// the user changed it with + / −), but no more than 90% of the screen along the side that reaches
// the edge first. Other content (text, PDF, sound, ...) gets a base size times its own remembered
// scale (preview.quick_look_doc_scale), its text scaled alike.
void QuickLookWindow::fitToContent(const QSize &natural)
{
    if (m_sizeSave->isActive())
        rememberUserSize(); // a drag that just ended belongs to what was shown before
    m_natural = natural;
    // Always measured against the monitor the browser window is on (with two monitors the
    // panel's own screen() is the primary display until it is shown, or wherever it was dragged).
    QWidget *browser = parentWidget() ? parentWidget()->window() : nullptr;
    QScreen *scr = browser ? QGuiApplication::screenAt(browser->geometry().center()) : nullptr;
    if (!scr && browser)
        scr = browser->screen();
    if (!scr)
        scr = screen();
    if (!scr)
        return;
    QPoint center = isVisible() ? geometry().center() : m_center;
    if (!scr->geometry().contains(center))
        center = browser ? browser->geometry().center() : scr->availableGeometry().center();
    const QRect avail = scr->availableGeometry();
    // Text, PDF, sound and the rest: the size the user gave that kind, else a base size.
    QSize want = windowSizeFor(m_preview->isAudio()).boundedTo(avail.size() * 0.9);
    QSize content; // the size the image/video should get
    if (natural.isValid()) {
        // A shown window measures what surrounds the image/video; a window that hasn't been
        // shown yet has no layout to measure, so it adds up the parts.
        QSize chrome;
        if (isVisible()) {
            layout()->activate();
            const QSize vp = m_preview->contentViewportSize();
            if (vp.isValid())
                chrome = (size() - vp).expandedTo(QSize(0, 0));
        }
        // The side that reaches the screen edge first sets the limit: 90% of it.
        const qreal fit = qMin(qreal(avail.width()) / natural.width(), qreal(avail.height()) / natural.height());
        const qreal k = qMin(Settings::instance()->value(Settings::QuickLookScale).toInt() / 100.0, 0.9 * fit);
        m_preview->setFitLimit(k);
        const QSize c(qRound(natural.width() * k), qRound(natural.height() * k));
        if (!chrome.isValid())
            chrome = m_preview->chromeEstimate(qMax(c.width(), 360));
        want = (c + chrome).expandedTo(QSize(360, 240));
        content = c;
    }
    want = want.boundedTo(avail.size());
    QRect g(QPoint(), want);
    g.moveCenter(center);
    g.moveLeft(qBound(avail.left(), g.left(), avail.right() - g.width() + 1));
    g.moveTop(qBound(avail.top(), g.top(), avail.bottom() - g.height() + 1));
    m_appliedSize = g.size();
    setGeometry(g);
    // The chrome guessed above can be off (styles, fonts, a details line that wraps); measure the
    // real viewport once and correct, so the content gets exactly its size.
    if (content.isValid() && isVisible()) {
        layout()->activate();
        const QSize vp = m_preview->contentViewportSize();
        if (vp.isValid() && vp != content) {
            const QSize fixed = (g.size() + content - vp).boundedTo(avail.size()).expandedTo(QSize(360, 240));
            if (fixed != g.size()) {
                const QPoint c = g.center();
                g.setSize(fixed);
                g.moveCenter(c);
                g.moveLeft(qBound(avail.left(), g.left(), avail.right() - g.width() + 1));
                g.moveTop(qBound(avail.top(), g.top(), avail.bottom() - g.height() + 1));
                m_appliedSize = g.size();
                setGeometry(g);
            }
        }
    }
    if (m_showPending && natural.isValid())
        reveal();
}

void QuickLookWindow::present()
{
    if (isVisible() || !m_preview->sizePending()) {
        reveal();
        return;
    }
    // A video's size arrives with its first frame: appear then, at that size, rather than at a
    // default size that changes a moment later. A timeout covers files that never report one.
    m_showPending = true;
    const int token = ++m_presentToken;
    QTimer::singleShot(500, this, [this, token] {
        if (m_showPending && token == m_presentToken)
            reveal();
    });
}

void QuickLookWindow::cancelPresent()
{
    if (!m_showPending)
        return;
    m_showPending = false;
    m_preview->stopMedia();
}

void QuickLookWindow::reveal()
{
    m_showPending = false;
    show();
    raise();
    activateWindow();
}

void QuickLookWindow::placeAround(const QPoint &center)
{
    m_center = center;
    QRect g = geometry();
    g.moveCenter(center);
    setGeometry(g);
}

void QuickLookWindow::showPath(const QString &path)
{
    m_preview->setPath(path);
    setWindowTitle(util::displayName(path));
    setWindowFilePath(path);
}

namespace {
// + / = (also with ⌘, Ctrl on Windows/Linux) enlarge, − shrink. Returns the factor, or 0.
qreal zoomKeyFactor(const QKeyEvent *e)
{
    QKeyEvent normalized(e->type(), e->key(),
                         (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal) ? e->modifiers() & ~Qt::ShiftModifier : e->modifiers(), e->text());
    if (Shortcuts::instance()->matches(QStringLiteral("퀵 뷰어 확대"), &normalized))
        return 1.25;
    if (Shortcuts::instance()->matches(QStringLiteral("퀵 뷰어 축소"), e))
        return 0.8;
    return 0;
}
} // namespace

// While Quick Look is up, Space/Esc always close it and arrows always move to the next item,
// even when a text or PDF view inside has focus (where Space would otherwise page down and
// arrows would scroll). PgUp/PgDn and the wheel still scroll the content.
void QuickLookWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
#ifdef Q_OS_MACOS
    macDisableWindowAnimation(this); // appear and vanish at once, no fade
#endif
    qApp->installEventFilter(this);
}

void QuickLookWindow::hideEvent(QHideEvent *e)
{
    if (m_sizeSave->isActive())
        rememberUserSize(); // closed right after a drag
    qApp->removeEventFilter(this);
    unsetZoomCursors();
    QWidget::hideEvent(e);
}

// The magnifier set on the image could stay on screen after the window closed under the
// pointer (macOS keeps the last cursor until something sets another one).
void QuickLookWindow::unsetZoomCursors()
{
    for (ZoomArea *a : findChildren<ZoomArea *>()) {
        a->viewport()->unsetCursor();
        if (a->widget())
            a->widget()->unsetCursor();
    }
#ifdef Q_OS_MACOS
    macResetCursor();
#endif
}

bool QuickLookWindow::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() != QEvent::KeyPress && ev->type() != QEvent::ShortcutOverride)
        return false;
    auto *w = qobject_cast<QWidget *>(obj);
    if (!w || (w != this && !isAncestorOf(w)))
        return false;
    auto *ke = static_cast<QKeyEvent *>(ev);
    if (ev->type() == QEvent::ShortcutOverride) {
        // Claim our keys so child widgets' and the main window's shortcuts don't run.
        if (zoomKeyFactor(ke)) {
            ke->accept();
            return true;
        }
        for (const Shortcuts::Entry &entry : Shortcuts::entries())
            if (entry.context.startsWith(QLatin1String("preview")) && Shortcuts::instance()->matches(entry.id, ke)) {
                ke->accept();
                return true;
            }
        return false;
    }
    return handleKey(ke);
}


bool QuickLookWindow::handleKey(QKeyEvent *e)
{
    if (const qreal f = zoomKeyFactor(e)) {
        // Images and videos: their scale; anything else: the window and its text. Each is
        // remembered for the next file of its kind; the window follows, up to 90% of the screen
        // (an image or video at that limit keeps its setting; text keeps growing inside).
        Settings *s = Settings::instance();
        const bool media = m_natural.isValid();
        if (!media && m_preview->isAudio()) { // sound: only its window, remembered as a size
            QSize next = (QSizeF(size()) * f).toSize().expandedTo(QSize(360, 160));
            if (screen())
                next = next.boundedTo(screen()->availableGeometry().size() * 0.9);
            s->setValue(Settings::QuickLookAudioWidth, next.width());
            s->setValue(Settings::QuickLookAudioHeight, next.height());
            fitToContent(m_natural);
            return true;
        }
        const char *key = media ? Settings::QuickLookScale : Settings::QuickLookDocScale;
        const int lo = media ? 25 : 50, hi = media ? 400 : 300;
        const int pct = s->value(QString::fromLatin1(key)).toInt();
        const QSize size = media ? m_natural : docBaseSize(m_preview->isAudio());
        if (media && f > 1 && screen()) { // the text of a capped window can still grow
            const QSize avail = screen()->availableGeometry().size();
            const qreal limit = 0.9 * qMin(qreal(avail.width()) / size.width(), qreal(avail.height()) / size.height());
            if (pct / 100.0 >= limit)
                return true;
        }
        int next = qBound(lo, int(std::lround(pct * f / 5.0) * 5), hi);
        if (next == pct)
            next = qBound(lo, pct + (f > 1 ? 5 : -5), hi);
        if (!media && pct > 0) { // a dragged document window grows with its text
            const int w = s->value(Settings::QuickLookDocWidth).toInt(), h = s->value(Settings::QuickLookDocHeight).toInt();
            if (w > 0 && h > 0) {
                s->setValue(Settings::QuickLookDocWidth, qRound(w * qreal(next) / pct));
                s->setValue(Settings::QuickLookDocHeight, qRound(h * qreal(next) / pct));
            }
        }
        s->setValue(QString::fromLatin1(key), next);
        fitToContent(m_natural);
        return true;
    }
    const auto matches = [e](const char *id) { return Shortcuts::instance()->matches(QString::fromUtf8(id), e); };
    if (matches("퀵 뷰어 닫기")) {
        close();
        return true;
    }
    if (m_preview->isPlayingMedia() && (matches("미디어 뒤로 탐색") || matches("미디어 앞으로 탐색"))) {
        m_preview->seekBy(matches("미디어 뒤로 탐색") ? -kSeekStepMs : kSeekStepMs);
        return true;
    }
    if (matches("퀵 뷰어 이전 항목") || matches("퀵 뷰어 다음 항목")) {
        if (m_target) {
            QKeyEvent copy(QEvent::KeyPress, matches("퀵 뷰어 이전 항목") ? Qt::Key_Up : Qt::Key_Down, Qt::NoModifier);
            QCoreApplication::sendEvent(m_target, &copy);
        }
        return true;
    }
    if (matches("미디어 재생·일시정지")) {
        m_preview->togglePlay();
        return true;
    }
    return false;
}

void QuickLookWindow::closeEvent(QCloseEvent *e)
{
    m_preview->stopMedia();
    QWidget::closeEvent(e);
    emit closed();
}
