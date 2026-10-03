#pragma once

#include <QImage>
#include <QScrollArea>
#include <QWidget>

class QLabel;
class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QMovie;
class QPlainTextEdit;
class QSlider;
class QStackedWidget;
class QToolButton;
class QPdfDocument;
class QPdfView;
class SystemPreview;

// Shows one image or video, by default fitted to the viewport but never above the fit limit
// (1.0: shrunk when larger, never enlarged; Quick Look sets the size it chose). A click toggles
// between that and 100%; the cursor is a magnifier showing which way the click goes.
// When the content is larger than the viewport, scrolling or dragging pans it.
class ZoomArea : public QScrollArea {
    Q_OBJECT
public:
    explicit ZoomArea(QWidget *content, QWidget *parent = nullptr);
    void setNaturalSize(const QSize &points); // invalid: content fills the viewport; resets to fit
    QSize naturalSize() const { return m_natural; }
    void setFitLimit(qreal maxScale);
    qreal scale() const;
    bool isActualSize() const { return !m_fit && qFuzzyCompare(m_scale, 1.0); }
    void setActualSize(bool on, const QPoint &anchor = {}); // anchor: viewport point kept in place
    bool isZoomable() const; // a click would change the size

signals:
    void scaleChanged(qreal scale);

protected:
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    qreal fitScale() const;
    qreal clickTarget() const; // scale a click switches to; <= 0 for "fit"
    void apply(bool fit, qreal scale, const QPoint &anchor);
    void relayout();
    void updateCursor();
    bool canPan() const;

    QWidget *m_content;
    QSize m_natural;
    qreal m_fitLimit = 1.0;
    bool m_fit = true;
    qreal m_scale = 1.0;
    bool m_pressed = false;
    bool m_panned = false;
    QPoint m_pressPos;
    QPoint m_scrollAtPress;
};

// Renders a preview of any path: images, animated GIFs, video/audio, text, PDF; office documents and
// whatever else Qt can't draw through the system's own preview (SystemPreview); else an info card.
// Used both by the Quick Look window (large, autoplay) and the in-window preview pane.
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    enum Style { QuickLook, Pane };
    explicit PreviewWidget(Style style, QWidget *parent = nullptr);
    ~PreviewWidget() override;
    // The media backend, before the first player exists (main() and the tests).
    static void chooseMediaBackend();

    void setPath(const QString &path);
    QString path() const { return m_path; }
    void stopMedia();
    // Forgets the remembered playing position unless that file is in this folder.
    void forgetPlaybackOutside(const QString &folder);
    void togglePlay();
    bool isPlayingMedia() const;
    bool isAudio() const; // sound (no picture) is shown
    void seekBy(qint64 ms); // sound/video: moves the playing position
    // The image/video's largest fitted scale (Quick Look: the size it chose for the window).
    void setFitLimit(qreal maxScale);
    // Viewport available to an image/video, for sizing the Quick Look window around it.
    QSize contentViewportSize() const;
    qreal contentScale() const; // the image/video's shown scale against its natural size, 0 for others
    // An image/video whose size isn't known yet (a video before its first frame, an image whose
    // header gives none); naturalSizeChanged follows.
    bool sizePending() const { return m_sizePending; }
    // What the window adds around contentViewportSize() at the given window width, computed
    // without a layout pass.
    QSize chromeEstimate(int width) const;

signals:
    // Natural size (points) of the image/video now shown; invalid for other kinds of content.
    void naturalSizeChanged(const QSize &points);

protected:
    void hideEvent(QHideEvent *e) override;

private:
    void ensureMedia();
    void ensurePdf();
    void showInfo(const QString &note = {});
    void showImage(const QString &path);
    bool showText(const QString &path);
    void showMedia(const QString &path, bool video);
    void showPdf(const QString &path);
    bool showDocument(const QString &path);
    bool showSystem(const QString &path);
    bool convertWithOffice(const QString &path);
    void loadFullImage();
    void setVideoNaturalSize(const QSize &pixels);
    void beginResume(qint64 target);
    void endResume(bool restorePause);
    void updateDetails();

    Style m_style;
    QString m_path;
    int m_generation = 0;

    QStackedWidget *m_stack;
    QLabel *m_image;
    ZoomArea *m_imageArea;
    QSize m_imagePixels;     // full size of the image file
    QSize m_imageLoaded;     // size of the decoded image (smaller for very large files)
    QSize m_resolution;      // pixels of the image/video shown, for the details line
    bool m_loadingFull = false;
    bool m_sizePending = false;
    QMovie *m_movie = nullptr;

    QWidget *m_mediaPage = nullptr;
    QVideoWidget *m_video = nullptr;
    ZoomArea *m_videoArea = nullptr;
    QLabel *m_audioIcon = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QToolButton *m_play = nullptr;
    QSlider *m_seek = nullptr;
    QLabel *m_time = nullptr;
    QSlider *m_volume = nullptr;
    // The file last played and where it stopped (closed or left before its end): showing it
    // again goes on from there. Cleared at the end, or when the browser leaves its folder.
    QString m_resumePath;
    qint64 m_resumePos = 0;
    qint64 m_pendingSeek = 0; // applied once the file has loaded
    qint64 m_seekTarget = -1;  // going there, unseen (beginResume)
    qint64 m_seekFloor = -1;   // its whole second, until the position moves
    bool m_resumePaused = false;

    QPlainTextEdit *m_text;
#ifdef HAVE_QTPDF
    QPdfDocument *m_pdfDoc = nullptr;
    QPdfView *m_pdf = nullptr;
#endif
    SystemPreview *m_system = nullptr; // the OS's own preview, created on first use
    QWidget *m_infoPage;
    QLabel *m_bigIcon;
    QLabel *m_infoNote;

    QLabel *m_title;
    QLabel *m_details;
};

// Floating Quick Look panel. Space/Esc close it; arrow keys are forwarded to the browser view
// so the preview follows the selection.
class QuickLookWindow : public QWidget {
    Q_OBJECT
public:
    explicit QuickLookWindow(QWidget *parent);
    void showPath(const QString &path);
    PreviewWidget *preview() const { return m_preview; }
    void setForwardTarget(QWidget *w) { m_target = w; }
    // Where the window appears (its size comes from the content and the remembered scales).
    void placeAround(const QPoint &center);
    // Shows the window; when the content's size is still on its way, waits for it (briefly).
    void present();
    void cancelPresent();
    bool isOpen() const { return isVisible() || m_showPending; }

signals:
    void closed();

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    bool handleKey(QKeyEvent *e);
    void fitToContent(const QSize &natural);
    QSize windowSizeFor(bool audio) const; // text, PDF, sound, …: remembered or base size
    void rememberUserSize();
    void unsetZoomCursors();
    void reveal();
    PreviewWidget *m_preview;
    QWidget *m_target = nullptr;
    QSize m_natural; // of the image/video shown, for re-sizing on + / −
    QPoint m_center;
    QSize m_appliedSize;           // the size the window was given; any other came from the user
    QTimer *m_sizeSave = nullptr;  // remembers a dragged size once the drag pauses
    QSize m_dragFrom;              // window size and image scale when the drag began
    qreal m_dragScale = 1.0;
    bool m_showPending = false;
    int m_presentToken = 0;
};
