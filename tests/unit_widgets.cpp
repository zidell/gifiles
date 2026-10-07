// Component tests: one widget at a time (the preview, the terminal, the sidebar, the settings
// window, the "다음으로 열기" dialog), without a MainWindow. Like every suite they run on Qt's
// offscreen platform (tests/Headless.h): no window reaches the screen or takes the keyboard.
//
// Untranslated (Korean source text), with GIFILES_CONFIG_DIR in a temporary folder.

#include "Headless.h"

#include "App.h"
#include "Log.h"
#include "OpenWith.h"
#include "Permissions.h"
#include "Preview.h"
#include "Pty.h"
#include "RecentFolders.h"
#include "Settings.h"
#include "Shortcuts.h"
#include "Sidebar.h"
#include "SystemPreview.h"
#include "TerminalWidget.h"
#include "Theme.h"
#include "Thumbnails.h"
#include "Util.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QColorDialog>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QDataStream>
#include <QImageWriter>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QMimeData>
#include <QSignalSpy>
#include <QTimer>
#include <QPainter>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QScrollBar>
#include <QSlider>
#include <QStackedWidget>
#include <QStyleOptionMenuItem>
#include <QTemporaryDir>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>
#include <QWheelEvent>
#include <QtTest>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifiers kTerminalMods = Qt::MetaModifier; // the Control key
#else
constexpr Qt::KeyboardModifiers kTerminalMods = Qt::ControlModifier;
#endif

class Widgets : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QTemporaryDir m_cfg;
    QWidget *m_win = nullptr; // parent of the dialogs, as the main window would be
    Sidebar *m_sidebar = nullptr;

    QString p(const QString &rel) const { return QDir(m_tmp.path()).filePath(rel); }
    QString np(const QString &rel) const { return QDir::toNativeSeparators(p(rel)); }
    void write(const QString &rel, const QByteArray &data)
    {
        QFile f(p(rel));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }
    void shot(const QString &name, QWidget *w)
    {
        if (const QString out = qEnvironmentVariable("OUT"); !out.isEmpty()) {
            QDir().mkpath(out);
            w->grab().save(QDir(out).filePath(name + QStringLiteral(".png")));
        }
    }
    static bool showsText(QWidget *w, const QString &text)
    {
        for (QLabel *l : w->findChildren<QLabel *>())
            if (l->isVisibleTo(w) && l->text().contains(text))
                return true;
        return false;
    }
    static QPushButton *buttonNamed(QWidget *w, const QString &text)
    {
        for (QPushButton *b : w->findChildren<QPushButton *>())
            if (b->text().remove(QLatin1Char('&')) == text)
                return b;
        return nullptr;
    }
    // Replaces a file the way an editor does (Windows can hold it a moment after the app's own save).
    static bool replaceFile(const QString &path, const QByteArray &data)
    {
        for (int attempt = 0; attempt < 30; ++attempt) {
            QSaveFile out(path);
            if (out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.commit())
                return true;
            QTest::qWait(100);
        }
        return false;
    }
    // Seconds of silence (8 kHz, 8-bit mono WAV).
    static QByteArray silentWav(int seconds)
    {
        const int rate = 8000, len = rate * seconds;
        QByteArray wav;
        QDataStream ds(&wav, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds.writeRawData("RIFF", 4);
        ds << quint32(36 + len);
        ds.writeRawData("WAVEfmt ", 8);
        ds << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate) << quint16(1) << quint16(8);
        ds.writeRawData("data", 4);
        ds << quint32(len);
        wav.append(QByteArray(len, char(0x80)));
        return wav;
    }
    // Answers the next QMessageBox::question (a modal loop of its own) with the given button.
    void answerNextQuestion(QMessageBox::StandardButton answer)
    {
        auto *timer = new QTimer(this);
        auto tries = std::make_shared<int>(0);
        connect(timer, &QTimer::timeout, this, [timer, tries, answer] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box || ++*tries > 250) {
                timer->stop();
                timer->deleteLater();
                if (box)
                    box->button(answer)->click();
            }
        });
        timer->start(20);
    }
    QWidget *settingsAt(const QString &page)
    {
        SettingsDialog::showSingleton(m_win, page);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<SettingsDialog *>(w) && w->isVisible())
                return w;
        return nullptr;
    }
    static void useTestShell()
    {
#if defined(Q_OS_MACOS)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/sh")); // no user rc files
#elif !defined(Q_OS_WIN)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/bash")); // /bin/sh may be dash: no $'...'
#endif
    }

private slots:
    void initTestCase()
    {
        PreviewWidget::chooseMediaBackend(); // as main() does
        qputenv("GIFILES_CONFIG_DIR", m_cfg.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("gifiles-widgets"));
        QSettings().clear();
        Theme::instance()->install();
        QVERIFY(m_tmp.isValid());
        for (const char *d : {"Alpha", "Beta", "감마"})
            QVERIFY(QDir().mkpath(p(QString::fromUtf8(d))));
        write(QStringLiteral("README.md"), "# Title\n");
#ifdef Q_OS_MACOS
        // The offscreen platform runs Qt's own event loop, not Cocoa's: AVFoundation's callbacks
        // (on the main dispatch queue) would never arrive and the media tests could only skip. Run
        // the CoreFoundation run loop briefly from a timer, as the Cocoa platform's loop would.
        auto *cf = new QTimer(this);
        connect(cf, &QTimer::timeout, this, [] { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true); });
        cf->start(5);
#endif
        m_win = new QWidget;
        m_win->resize(900, 600);
        m_sidebar = new Sidebar(m_win);
        m_sidebar->setGeometry(0, 0, 260, 600);
        m_win->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
    }

    void cleanupTestCase() { delete m_win; }

    void cleanup() { Settings::instance()->setValue(Settings::TermShell, QString()); }

    void selectionHighlightUsesWindowsTone()
    {
#ifdef Q_OS_WIN
        QVERIFY(Theme::colors().selection.value() < Theme::colors().accent.value());
#elif defined(Q_OS_LINUX)
        if (Theme::colors().dark) {
            const QColor accent = Theme::colors().accent;
            QCOMPARE(Theme::colors().selection, QColor(qRound(accent.red() * 0.68), qRound(accent.green() * 0.68), qRound(accent.blue() * 0.68), accent.alpha()));
        } else {
            QCOMPARE(Theme::colors().selection, Theme::colors().accent);
        }
#else
        QCOMPARE(Theme::colors().selection, Theme::colors().accent);
#endif
    }

    void fileSelectionColorFollowsPlatformAndMode()
    {
        const QVariant original = Settings::instance()->value(Settings::ThemeMode);
        const auto restore = qScopeGuard([original] { Settings::instance()->setValue(Settings::ThemeMode, original); });
        Settings::instance()->setValue(Settings::ThemeMode, QStringLiteral("light"));
        QTRY_VERIFY(!Theme::colors().dark);
        const QColor accent = Theme::colors().accent;
#ifdef Q_OS_WIN
        QCOMPARE(Theme::colors().selection, accent.darker(135));
#else
        QCOMPARE(Theme::colors().selection, accent);
#endif
        Settings::instance()->setValue(Settings::ThemeMode, QStringLiteral("dark"));
        QTRY_VERIFY(Theme::colors().dark);
        QCOMPARE(Theme::colors().accent, accent);
        QCOMPARE(Theme::colors().selText, QColor(Qt::white));
#ifdef Q_OS_LINUX
        const QColor selected = Theme::colors().selection;
        QVERIFY(qAbs(selected.red() - accent.red() * 0.68) <= 0.5);
        QVERIFY(qAbs(selected.green() - accent.green() * 0.68) <= 0.5);
        QVERIFY(qAbs(selected.blue() - accent.blue() * 0.68) <= 0.5);
#elif defined(Q_OS_WIN)
        QCOMPARE(Theme::colors().selection, accent.darker(135));
#else
        QCOMPARE(Theme::colors().selection, accent);
#endif
    }

    void appIconResourceRenders()
    {
        const QIcon icon(QStringLiteral(":/gifiles.svg"));
        QVERIFY(!icon.isNull());
        for (int size : {16, 32, 64, 256})
            QVERIFY(!icon.pixmap(size, size).isNull());
    }

    void menuSelectionRendersWithReadableBackground()
    {
        const QVariant original = Settings::instance()->value(Settings::ThemeMode);
        const auto restore = qScopeGuard([original] { Settings::instance()->setValue(Settings::ThemeMode, original); });
        QMenu general;
        Theme::ShortcutMenu context;
        QMenu parent;
        QMenu *submenu = parent.addMenu(QStringLiteral("Submenu"));
        for (const QString &mode : {QStringLiteral("light"), QStringLiteral("dark")}) {
            Settings::instance()->setValue(Settings::ThemeMode, mode);
            QTRY_COMPARE(Theme::colors().dark, mode == QStringLiteral("dark"));
            const auto &colors = Theme::colors();
            QColor expected = colors.accent;
#ifdef Q_OS_WIN
            if (colors.dark)
                expected = colors.nameSelection;
#endif
            // QSS serializes colors to 8-bit channels; verify the painted row, not only the palette.
            expected = QColor(expected.name());
            for (QMenu *menu : {&general, static_cast<QMenu *>(&context), submenu}) {
                menu->clear();
                QAction *action = menu->addAction(QStringLiteral("Readable label"));
                action->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
                menu->ensurePolished();
                menu->resize(menu->sizeHint());
                menu->setActiveAction(action);
                const QRect row = menu->actionGeometry(action);
                const QImage image = menu->grab().toImage();
                const qreal dpr = menu->devicePixelRatioF();
                QCOMPARE(image.pixelColor(qRound((row.left() + 7) * dpr), qRound(row.center().y() * dpr)), expected);
                shot(mode + (menu == &context ? QStringLiteral("-context-menu")
                             : menu == submenu ? QStringLiteral("-submenu") : QStringLiteral("-general-menu")), menu);
            }
        }
    }

    void menuShortcutTextHasHalfOpacity()
    {
        class Menu : public Theme::ShortcutMenu {
        public:
            using QMenu::initStyleOption;
        } menu;
        QAction *action = menu.addAction(QStringLiteral("Label"));
        action->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
        menu.ensurePolished();
        menu.resize(menu.sizeHint());
        for (bool selected : {false, true}) {
            menu.setActiveAction(selected ? action : nullptr);
            QStyleOptionMenuItem item;
            menu.initStyleOption(&item, action);
            const QRect row = menu.actionGeometry(action);
            auto render = [&](bool shortcut) {
                QImage result(row.size(), QImage::Format_ARGB32_Premultiplied);
                result.fill(Theme::colors().menuBg);
                QPainter painter(&result);
                painter.setFont(menu.font());
                QStyleOptionMenuItem option(item);
                option.rect = QRect(QPoint(), row.size());
                if (!shortcut)
                    option.text = option.text.left(option.text.indexOf(QLatin1Char('\t')) + 1);
                menu.style()->drawControl(QStyle::CE_MenuItem, &option, &painter, &menu);
                return result;
            };
            const QImage full = render(true), bare = render(false);
            const QImage actual = menu.grab(row).toImage();
            int shortcutPixels = 0;
            for (int y = 0; y < full.height(); ++y)
                for (int x = 0; x < full.width(); ++x) {
                    const QRgb a = full.pixel(x, y), b = bare.pixel(x, y);
                    const QRgb expected = qRgba((qRed(a) + qRed(b)) / 2, (qGreen(a) + qGreen(b)) / 2,
                                                (qBlue(a) + qBlue(b)) / 2, (qAlpha(a) + qAlpha(b)) / 2);
                    QCOMPARE(actual.pixel(x, y), expected);
                    shortcutPixels += a != b;
                }
            QVERIFY(shortcutPixels > 20);
        }
    }

    void previewFontSizesPerKind()
    {
        // Documents (txt, md) use the UI font at the document size, other text the fixed-width font.
        write(QStringLiteral("notes.md"), "# Notes\nsome prose\n");
        write(QStringLiteral("main.py"), "print('hi')\n");
        Settings::instance()->setValue(Settings::PreviewDocFontSize, 17);
        Settings::instance()->setValue(Settings::PreviewTextFontSize, 11);
        PreviewWidget pw(PreviewWidget::Pane);
        pw.resize(400, 300);
        auto *text = pw.findChild<QPlainTextEdit *>();
        QVERIFY(text);
        pw.setPath(p("notes.md"));
        QCOMPARE(text->font().pointSize(), 17);
        QCOMPARE(text->font().family(), QApplication::font().family());
        QCOMPARE(text->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        Settings::instance()->setValue(Settings::PreviewDocFontSize, 19); // applies to what is shown
        QCOMPARE(text->font().pointSize(), 19);
        pw.setPath(p("main.py"));
        QCOMPARE(text->font().pointSize(), 11);
        QCOMPARE(text->font().family(), QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
        QCOMPARE(text->lineWrapMode(), QPlainTextEdit::NoWrap);
        Settings::instance()->setValue(Settings::PreviewDocFontSize, 14);
        Settings::instance()->setValue(Settings::PreviewTextFontSize, 12);
        QFile::remove(p("notes.md"));
        QFile::remove(p("main.py"));
    }

    void officeDocumentsUseTheSystemPreview()
    {
        // Word/Excel/… go to the system's preview (Quick Look, a preview handler, LibreOffice on
        // Linux); without one (the offscreen platform) an info card, never the zip's bytes as text.
        QVERIFY(SystemPreview::isOfficeDocument(QStringLiteral("a/보고서.DOCX")));
        QVERIFY(SystemPreview::isOfficeDocument(QStringLiteral("b.xlsx")));
        QVERIFY(SystemPreview::isOfficeDocument(QStringLiteral("c.pages")));
        QVERIFY(!SystemPreview::isOfficeDocument(QStringLiteral("d.txt")));
        QVERIFY(!SystemPreview::available()); // tests run offscreen
        write(QStringLiteral("report.docx"), QByteArray("PK\x03\x04", 4) + QByteArray(64, '\0'));
        PreviewWidget pw(PreviewWidget::Pane);
        pw.setPath(p("report.docx"));
        auto *stack = pw.findChild<QStackedWidget *>();
        QVERIFY(stack && !qobject_cast<QPlainTextEdit *>(stack->currentWidget()));
        QFile::remove(p("report.docx"));
    }

    void previewShutdownStopsProcessCallbacks()
    {
        auto *pw = new PreviewWidget(PreviewWidget::Pane);
        auto *proc = new QProcess(pw);
        int callbacks = 0;
        connect(proc, &QProcess::finished, pw, [&callbacks] { ++callbacks; });
        proc->start(QCoreApplication::applicationFilePath(), {QStringLiteral("-help")});
        QVERIFY(proc->waitForStarted());
        delete pw; // conversion shutdown must not call into partially destroyed child widgets
        QCOMPARE(callbacks, 0);
    }

    void previewShowsEveryKindOrSaysWhy()
    {
        // Empty files, broken pictures and PDFs say so; a PDF, an animated GIF and a folder are shown;
        // a long text is cut with a note; a zoomed picture can be dragged around.
        QVERIFY(QDir().mkpath(p(QStringLiteral("kinds"))));
        const auto restore = qScopeGuard([this] { QDir(p("kinds")).removeRecursively(); });
        write(QStringLiteral("kinds/empty.txt"), QByteArray());
        write(QStringLiteral("kinds/broken.png"), QByteArray("\x89PNG\r\n\x1a\nnot really", 18));
        write(QStringLiteral("kinds/broken.pdf"), QByteArray("not a pdf at all"));
        write(QStringLiteral("kinds/long.txt"), QByteArray(600 * 1024, 'a'));
        {
            QPdfWriter pdf(p("kinds/doc.pdf"));
            QPainter painter(&pdf);
            painter.drawText(100, 100, QStringLiteral("hello pdf"));
        }
        // Two 1×1 frames.
        const QByteArray frame = QByteArray::fromHex("21f904000a0000002c0000000001000100000202440100");
        write(QStringLiteral("kinds/anim.gif"),
              QByteArray::fromHex("47494638396101000100800000000000ffffff") + frame + frame + QByteArray("\x3b", 1));
        QImage big(2400, 1800, QImage::Format_RGB32);
        big.fill(Qt::darkYellow);
        QVERIFY(big.save(p("kinds/big.png")));

        PreviewWidget pw(PreviewWidget::QuickLook);
        pw.resize(500, 400);
        pw.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pw));
        auto *stack = pw.findChild<QStackedWidget *>();
        QVERIFY(stack);
        pw.setPath(p("kinds/empty.txt"));
        QTRY_VERIFY(showsText(&pw, Gifiles::tr("빈 파일")));
        pw.setPath(p("kinds/broken.png"));
        QTRY_VERIFY(showsText(&pw, Gifiles::tr("이미지를 열 수 없습니다")));
        pw.setPath(p("kinds/long.txt"));
        auto *text = pw.findChild<QPlainTextEdit *>();
        QTRY_VERIFY(text && text->isVisible());
        QVERIFY(text->toPlainText().endsWith(Gifiles::tr("\n\n— 처음 %1만 표시 —").arg(util::humanSize(512 * 1024))));
        pw.setPath(p("kinds"));
        QTRY_VERIFY(showsText(&pw, Gifiles::tr("%1개 항목").arg(QDir(p("kinds")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size())));
#ifdef HAVE_QTPDF
        pw.setPath(p("kinds/broken.pdf"));
        QTRY_VERIFY(showsText(&pw, Gifiles::tr("PDF를 열 수 없습니다")));
        pw.setPath(p("kinds/doc.pdf"));
        QTRY_VERIFY(stack->currentWidget() && stack->currentWidget()->inherits("QPdfView"));
#endif
        pw.setPath(p("kinds/anim.gif"));
        ZoomArea *area = pw.findChild<ZoomArea *>();
        QVERIFY(area);
        QTRY_COMPARE(stack->currentWidget(), static_cast<QWidget *>(area));
        auto *image = qobject_cast<QLabel *>(area->widget());
        QVERIFY(image);
        QTRY_VERIFY(image->movie()); // played, not a still
        QCOMPARE(area->naturalSize(), QSize(1, 1));

        pw.setPath(p("kinds/big.png"));
        QTRY_COMPARE(area->naturalSize(), big.size());
        QTRY_VERIFY(!image->pixmap().isNull());
        QTest::mouseClick(area->widget(), Qt::LeftButton, {}, area->widget()->rect().center()); // 100%
        QTRY_VERIFY(area->isActualSize());
        const QPoint start = area->viewport()->rect().center();
        const int h0 = area->horizontalScrollBar()->value(), v0 = area->verticalScrollBar()->value();
        QTest::mousePress(area->viewport(), Qt::LeftButton, {}, start);
        QTest::mouseMove(area->viewport(), start - QPoint(30, 20));
        QTest::mouseMove(area->viewport(), start - QPoint(80, 60));
        QTest::mouseRelease(area->viewport(), Qt::LeftButton, {}, start - QPoint(80, 60));
        QTRY_COMPARE(area->horizontalScrollBar()->value(), h0 + 80); // the picture follows the hand
        QCOMPARE(area->verticalScrollBar()->value(), v0 + 60);
        QVERIFY(area->isActualSize()); // a drag is not a click: still 100%
        pw.setPath(QString());
    }

    // Quick Look on a sound: Return plays and pauses, ← / → go 5 s back and on (↑ / ↓ still move
    // the selection, forwarded to the list).
    void quickLookMediaKeys()
    {
        write(QStringLiteral("long.wav"), silentWav(20));
        const auto restore = qScopeGuard([this] { QFile::remove(p("long.wav")); });
        QuickLookWindow ql(m_win);
        QWidget list; // stands for the file list the arrows go to
        QStringList forwarded;
        struct Spy : QObject {
            QStringList *out;
            bool eventFilter(QObject *, QEvent *e) override
            {
                if (e->type() == QEvent::KeyPress)
                    *out << QKeySequence(static_cast<QKeyEvent *>(e)->key()).toString();
                return false;
            }
        } spy;
        spy.out = &forwarded;
        list.installEventFilter(&spy);
        ql.setForwardTarget(&list);
        ql.showPath(p("long.wav"));
        ql.present();
        QTRY_VERIFY(ql.isVisible());
        auto *player = ql.findChild<QMediaPlayer *>();
        QVERIFY(player);
        QTRY_VERIFY_WITH_TIMEOUT(player->duration() > 15000, 10000);
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState); // Quick Look plays at once
        QTest::keyClick(&ql, Qt::Key_Return);
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
        const qint64 at = player->position();
        QTest::keyClick(&ql, Qt::Key_Right);
        QTRY_VERIFY(qAbs(player->position() - (at + 5000)) < 300);
        QTest::keyClick(&ql, Qt::Key_Left);
        QTest::keyClick(&ql, Qt::Key_Left);
        QTRY_COMPARE(player->position(), qint64(0)); // not before the start
        QTest::keyClick(&ql, Qt::Key_Return);
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QTest::keyClick(&ql, Qt::Key_Down);
        QCOMPARE(forwarded, QStringList{QStringLiteral("Down")});
        QTest::keyClick(&ql, Qt::Key_Escape);
        QTRY_VERIFY(!ql.isVisible());
        QVERIFY(player->source().isEmpty()); // closing stops it
    }

    // The size a picture is shown at comes from its header before it is decoded: a 144 dpi JPEG
    // (JFIF) at half its pixels, a camera picture turned by its EXIF orientation.
    void previewImageSizeFromHeader()
    {
        QImage img(400, 200, QImage::Format_RGB32);
        img.fill(Qt::darkGreen);
        img.setDotsPerMeterX(5669); // 144 dpi
        img.setDotsPerMeterY(5669);
        QVERIFY(img.save(p("retina.jpg")));
        QImage plain(400, 200, QImage::Format_RGB32);
        plain.fill(Qt::darkRed);
        plain.setDotsPerMeterX(2835); // 72 dpi: as is
        plain.setDotsPerMeterY(2835);
        QVERIFY(plain.save(p("plain.jpg")));
        QImageWriter turned(p("turned.jpg"));
        turned.setTransformation(QImageIOHandler::TransformationRotate90);
        QVERIFY(turned.write(plain));
        const auto restore = qScopeGuard([this] {
            for (const char *f : {"retina.jpg", "plain.jpg", "turned.jpg"})
                QFile::remove(p(QString::fromLatin1(f)));
        });
        PreviewWidget pw(PreviewWidget::QuickLook);
        pw.resize(600, 400);
        auto *area = pw.findChild<ZoomArea *>();
        QVERIFY(area);
        pw.setPath(p("retina.jpg"));
        QCOMPARE(area->naturalSize(), QSize(200, 100));
        pw.setPath(p("plain.jpg"));
        QCOMPARE(area->naturalSize(), QSize(400, 200));
        pw.setPath(p("turned.jpg"));
        QCOMPARE(area->naturalSize(), QSize(200, 400)); // standing, as the camera held it
        pw.setPath(QString());
    }

    // Gallery thumbnails: made in the background, at most 256 px a side, a PDF's first page on white;
    // a file that can't be read is tried once; a changed file (new mtime) gets a new one.
    void thumbnailsInTheBackground()
    {
        Thumbnails *t = Thumbnails::instance();
        QVERIFY(QDir().mkpath(p("thumbs")));
        const auto restore = qScopeGuard([this] { QDir(p("thumbs")).removeRecursively(); });
        QImage big(1200, 600, QImage::Format_RGB32);
        big.fill(Qt::darkBlue);
        QVERIFY(big.save(p("thumbs/big.png")));
        write(QStringLiteral("thumbs/broken.jpg"), "not an image");
        {
            QPdfWriter pdf(p("thumbs/doc.pdf"));
            QPainter painter(&pdf);
            painter.drawText(100, 100, QStringLiteral("page one"));
        }
        QVERIFY(Thumbnails::canThumbnail(p("thumbs/big.png")));
        QVERIFY(Thumbnails::canThumbnail(p("thumbs/X.JPG")));
        QVERIFY(!Thumbnails::canThumbnail(p("thumbs/notes.txt")));
        QSignalSpy ready(t, &Thumbnails::ready);
        auto mtime = [this](const char *rel) { return QFileInfo(p(QString::fromLatin1(rel))).lastModified(); };
        QVERIFY(t->get(p("thumbs/big.png"), mtime("thumbs/big.png")).isNull()); // not yet: on its way
        QTRY_VERIFY(ready.contains(QVariantList{p("thumbs/big.png")}));
        const QPixmap pm = t->get(p("thumbs/big.png"), mtime("thumbs/big.png"));
        QCOMPARE(pm.size(), QSize(256, 128));
        QVERIFY(t->get(p("thumbs/broken.jpg"), mtime("thumbs/broken.jpg")).isNull());
        QTest::qWait(300);
        QVERIFY(!ready.contains(QVariantList{p("thumbs/broken.jpg")}));
        ready.clear();
        QVERIFY(t->get(p("thumbs/broken.jpg"), mtime("thumbs/broken.jpg")).isNull()); // not tried again
        QTest::qWait(200);
        QVERIFY(ready.isEmpty());
        // A new mtime is a new picture.
        QVERIFY(t->get(p("thumbs/big.png"), mtime("thumbs/big.png").addSecs(5)).isNull());
        QTRY_VERIFY(ready.contains(QVariantList{p("thumbs/big.png")}));
#ifdef HAVE_QTPDF
        QVERIFY(Thumbnails::canThumbnail(p("thumbs/doc.pdf")));
        QVERIFY(t->get(p("thumbs/doc.pdf"), mtime("thumbs/doc.pdf")).isNull());
        QTRY_VERIFY(ready.contains(QVariantList{p("thumbs/doc.pdf")}));
        const QImage page = t->get(p("thumbs/doc.pdf"), mtime("thumbs/doc.pdf")).toImage();
        QVERIFY(page.height() == 256 || page.width() == 256);
        QCOMPARE(page.pixelColor(2, 2), QColor(Qt::white)); // paper, not transparent
#endif
    }

    void mediaBarsJumpWhereClicked()
    {
        // A click on a bar's groove moves it there at once (not a page step); the volume is kept.
        write(QStringLiteral("tone.mp3"), QByteArray(256, '\0'));
        PreviewWidget pw(PreviewWidget::QuickLook);
        pw.resize(600, 300);
        pw.setPath(p("tone.mp3"));
        pw.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pw));
        auto *volume = pw.findChild<QSlider *>(QStringLiteral("volume"));
        QVERIFY(volume);
        volume->setValue(90);
        QTest::mouseClick(volume, Qt::LeftButton, {}, QPoint(volume->width() / 4, volume->height() / 2));
        QVERIFY2(volume->value() < 45, qPrintable(QString::number(volume->value())));
        QCOMPARE(Settings::instance()->value(Settings::MediaVolume).toInt(), volume->value());
        Settings::instance()->remove(Settings::MediaVolume);
        pw.close();
        QFile::remove(p("tone.mp3"));
    }

    void mediaResumesWhereItStopped()
    {
        // 10 s of silence (8 kHz, 8-bit mono WAV).
        const int rate = 8000, len = rate * 10;
        QByteArray wav;
        QDataStream ds(&wav, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds.writeRawData("RIFF", 4);
        ds << quint32(36 + len);
        ds.writeRawData("WAVEfmt ", 8);
        ds << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate) << quint16(1) << quint16(8);
        ds.writeRawData("data", 4);
        ds << quint32(len);
        wav.append(QByteArray(len, char(0x80)));
        write(QStringLiteral("quiet.wav"), wav);
        write(QStringLiteral("other.txt"), "x");

        PreviewWidget pw(PreviewWidget::Pane); // no autoplay
        pw.resize(500, 300);
        pw.show();
        auto loaded = [&](QMediaPlayer *pl) {
            for (int i = 0; i < 100 && pl->mediaStatus() != QMediaPlayer::LoadedMedia && pl->mediaStatus() != QMediaPlayer::BufferedMedia; ++i)
                QTest::qWait(50);
            return pl->mediaStatus() == QMediaPlayer::LoadedMedia || pl->mediaStatus() == QMediaPlayer::BufferedMedia;
        };
        pw.setPath(p("quiet.wav"));
        auto *player = pw.findChild<QMediaPlayer *>();
        QVERIFY(player);
        if (!loaded(player))
            QSKIP("the media backend didn't load the file (macOS offscreen: AVFoundation needs the Cocoa run loop)");
        player->setPosition(4000);
        QTRY_VERIFY(player->position() >= 3500);
        pw.setPath(p("other.txt"));
        pw.setPath(p("quiet.wav")); // back: goes on from 4 s
        QVERIFY(loaded(player));
        QTRY_VERIFY2(player->position() >= 3500, qPrintable(QString::number(player->position())));
        pw.setPath(p("other.txt"));
        pw.forgetPlaybackOutside(p("감마")); // the browser went to another folder
        pw.setPath(p("quiet.wav"));
        QVERIFY(loaded(player));
        QTest::qWait(200);
        QCOMPARE(player->position(), 0);
        // Played to the end: next time from the start.
        player->setPosition(9600);
        player->play();
        QTRY_COMPARE_WITH_TIMEOUT(player->mediaStatus(), QMediaPlayer::EndOfMedia, 5000);
        pw.setPath(p("other.txt"));
        pw.setPath(p("quiet.wav"));
        QVERIFY(loaded(player));
        QTest::qWait(200);
        QCOMPARE(player->position(), 0);
        pw.setPath(QString());
        QFile::remove(p("quiet.wav"));
        QFile::remove(p("other.txt"));
    }

    void videoResumesAtTheExactSpot()
    {
        // A fresh file seeks in whole seconds on macOS; going on must still start where it stopped.
        QFile::copy(QStringLiteral(GIFILES_SOURCE_DIR "/tests/data/resume.mp4"), p("resume.mp4"));
        write(QStringLiteral("other.txt"), "x");
        PreviewWidget pw(PreviewWidget::QuickLook); // autoplay
        pw.resize(400, 300);
        pw.show();
        pw.setPath(p("resume.mp4"));
        auto *player = pw.findChild<QMediaPlayer *>();
        auto *video = pw.findChild<QVideoWidget *>();
        QVERIFY(player && video);
        for (int i = 0; i < 100 && player->position() < 1500; ++i)
            QTest::qWait(30);
        if (player->position() < 1500)
            QSKIP("the media backend didn't play the file (macOS offscreen: AVFoundation needs the Cocoa run loop)");
        player->pause();
        player->setPosition(2450);
        QTRY_VERIFY(qAbs(player->position() - 2450) < 40);
        pw.setPath(p("other.txt"));
        qint64 firstShown = -1;
        connect(video->videoSink(), &QVideoSink::videoFrameChanged, &pw, [&](const QVideoFrame &f) {
            if (firstShown < 0 && video->isVisible() && player->mediaStatus() != QMediaPlayer::LoadingMedia)
                firstShown = f.startTime() >= 0 ? f.startTime() / 1000 : player->position(); // Qt 6.10 (macOS): frames carry no time
        });
        pw.setPath(p("resume.mp4"));
        QTRY_VERIFY(player->position() > 2600); // goes on playing from there
#ifdef Q_OS_MACOS
        QVERIFY2(firstShown >= 2400 && firstShown < 2600, qPrintable(QString::number(firstShown)));
#endif
        pw.setPath(QString());
        QFile::remove(p("resume.mp4"));
        QFile::remove(p("other.txt"));
    }

    void terminalShowsKoreanNames()
    {
        // Apps started from Finder/Dock have no locale variables; reproduce that.
        for (const char *v : {"LANG", "LC_ALL", "LC_CTYPE"})
            qunsetenv(v);
        write(QStringLiteral("한글파일.txt"), "x");
        TerminalWidget term;
        term.resize(900, 300);
        term.show();
        term.start(m_tmp.path()); // the user's real login shell
        QTRY_VERIFY2_WITH_TIMEOUT(term.isRunning(), qPrintable(term.screenText()), 5000);
        QTest::qWait(1500); // shell startup files
        term.setFocus();
#ifdef Q_OS_WIN
        QTest::keyClicks(&term, "Get-ChildItem -Name"); // one name per line; the default table wraps them
#else
        QTest::keyClicks(&term, "ls");
#endif
        QTest::keyClick(&term, Qt::Key_Return);
        QTest::qWait(1000);
        const QString screen = term.screenText().normalized(QString::NormalizationForm_C);
        QVERIFY2(screen.contains(QStringLiteral("한글파일.txt")), qPrintable(screen));
        QVERIFY(screen.contains(QStringLiteral("감마")));

        // Korean typed through an input method reaches the shell intact.
        QTest::keyClicks(&term, "echo ");
        QInputMethodEvent ime;
        ime.setCommitString(QStringLiteral("가나다"));
        QApplication::sendEvent(&term, &ime);
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY(term.screenText().normalized(QString::NormalizationForm_C).count(QStringLiteral("가나다")) >= 2);
    }

    void terminalBoxGlyphsFillTheCell()
    {
        // Border lines reach the cell's edges whatever the line height, so a column of │ stays one line.
        auto draw = [](char32_t ch) {
            QImage img(8, 20, QImage::Format_ARGB32);
            img.fill(Qt::transparent);
            QPainter p(&img);
            const bool drawn = TerminalWidget::drawBoxGlyph(p, QRectF(0, 0, 8, 20), ch, Qt::black);
            return std::make_pair(drawn, img);
        };
        auto inked = [](const QImage &img, int x, int y) { return qAlpha(img.pixel(x, y)) > 0; };
        const auto [vDrawn, v] = draw(U'│');
        QVERIFY(vDrawn);
        QVERIFY(inked(v, 4, 0) && inked(v, 4, 19) && !inked(v, 0, 10));
        const auto [hDrawn, hz] = draw(U'─');
        QVERIFY(hDrawn && inked(hz, 0, 10) && inked(hz, 7, 10) && !inked(hz, 4, 0));
        const auto [cDrawn, corner] = draw(U'╭'); // rounded: down to the bottom, out to the right
        QVERIFY(cDrawn && inked(corner, 4, 19) && inked(corner, 7, 10) && !inked(corner, 4, 0) && !inked(corner, 0, 10));
        const auto [bDrawn, block] = draw(U'█');
        QVERIFY(bDrawn && inked(block, 0, 0) && inked(block, 7, 19));
        QVERIFY(!draw(U'┄').first); // dashed lines and text keep the font's glyph
        QVERIFY(!draw(U'a').first);
    }

    // 설정 → 터미널: 글꼴 (terminal.font_family, empty = the bundled D2Coding) and 줄간격 (terminal.line_height, %) apply at
    // once; the input method's cursor rectangle is one cell.
    void terminalFontAndLineHeightSettings()
    {
        auto *s = Settings::instance();
        const auto restore = qScopeGuard([s] {
            s->setValue(Settings::TermFontFamily, QString());
            s->setValue(Settings::TermLineHeight, 100);
        });
        TerminalWidget term;
        term.resize(600, 400);
        QWidget *w = &term;
        auto cellHeight = [w] { return w->inputMethodQuery(Qt::ImCursorRectangle).toRect().height(); };
        const int normal = cellHeight();
        s->setValue(Settings::TermLineHeight, 200);
        QVERIFY2(cellHeight() >= 2 * normal - 1, qPrintable(QStringLiteral("%1 → %2").arg(normal).arg(cellHeight())));
        s->setValue(Settings::TermLineHeight, 100);
        QCOMPARE(cellHeight(), normal);
        const QString family = QStringLiteral("Gifiles Test Mono"); // QFont keeps the asked name (no font list offscreen on Windows)
        s->setValue(Settings::TermFontFamily, family);
        QCOMPARE(w->inputMethodQuery(Qt::ImFont).value<QFont>().family(), family);
        s->setValue(Settings::TermFontFamily, QString());
#ifdef Q_OS_WIN // Windows' offscreen platform has no font database to register it in: the system's fixed font
        QVERIFY(TerminalWidget::bundledFont().isEmpty());
        QCOMPARE(w->inputMethodQuery(Qt::ImFont).value<QFont>().family(), QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
#else
        QCOMPARE(TerminalWidget::bundledFont(), QStringLiteral("D2Coding"));
        QCOMPARE(w->inputMethodQuery(Qt::ImFont).value<QFont>().family(), QStringLiteral("D2Coding"));
#endif
    }

    void terminalKeepsSplitCharactersWhole()
    {
        // The end of a pty read can stop inside a character; those bytes wait for the next read.
        const QByteArray s = QStringLiteral("이동 ←").toUtf8(); // 3 + 3 + 1 + 3 bytes
        QCOMPARE(TerminalWidget::incompleteUtf8Tail(s), 0);
        QCOMPARE(TerminalWidget::incompleteUtf8Tail(s.left(4)), 1);  // "이" + first byte of "동"
        QCOMPARE(TerminalWidget::incompleteUtf8Tail(s.left(5)), 2);
        QCOMPARE(TerminalWidget::incompleteUtf8Tail(s.left(9)), 2); // "←" missing its last byte
        QCOMPARE(TerminalWidget::incompleteUtf8Tail(QByteArray("abc")), 0);
        QCOMPARE(TerminalWidget::incompleteUtf8Tail(QByteArray()), 0);
    }

    void terminalKeepsFaintText()
    {
#ifdef VTERM_HAS_DIM
        // The bundled libvterm is patched to keep SGR 2 (faint); upstream 0.3.3 drops it, so dimmed
        // text such as Claude Code's suggestions showed at full brightness.
        VTerm *vt = vterm_new(2, 20);
        vterm_set_utf8(vt, 1);
        VTermScreen *screen = vterm_obtain_screen(vt);
        vterm_screen_reset(screen, 1);
        const char s[] = "\x1b[2mab\x1b[22mc\x1b[1;2md\x1b[0me";
        vterm_input_write(vt, s, sizeof s - 1);
        VTermScreenCell cell;
        auto dim = [&](int col) { vterm_screen_get_cell(screen, VTermPos{0, col}, &cell); return bool(cell.attrs.dim); };
        QVERIFY(dim(0) && dim(1));
        QVERIFY(!dim(2)); // 22 ends faint
        QVERIFY(dim(3));
        QVERIFY(!dim(4)); // 0 resets
        vterm_free(vt);

#ifndef Q_OS_WIN
        // Drawn: a faint line has less contrast against the background than the same line without it.
        useTestShell();
        TerminalWidget term;
        term.resize(600, 200);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));
        term.start(m_tmp.path());
        QTRY_VERIFY2_WITH_TIMEOUT(term.isReady(), qPrintable(term.screenText()), 15000);
        term.setFocus();
        QTest::keyClicks(&term, "clear; printf '\\033[2mMMMMMMMM\\033[0m\\nMMMMMMMM\\n'; sleep 5");
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY2(term.screenText().startsWith(QStringLiteral("MMMMMMMM\nMMMMMMMM")), qPrintable(term.screenText()));
        QTest::qWait(100);
        const QImage img = term.grab().toImage();
        const int bgL = img.pixelColor(1, 1).lightness();
        QList<int> bands; // strongest contrast of each run of rows holding ink
        bool inBand = false;
        for (int y = 0; y < img.height(); ++y) {
            int best = 0;
            for (int x = 0; x < img.width() / 2; ++x)
                best = qMax(best, qAbs(img.pixelColor(x, y).lightness() - bgL));
            if (best > 20) {
                if (!inBand)
                    bands << 0;
                bands.last() = qMax(bands.last(), best);
            }
            inBand = best > 20;
        }
        QVERIFY2(bands.size() >= 2, qPrintable(QString::number(bands.size())));
        QVERIFY2(bands[0] < bands[1] * 3 / 4, qPrintable(QStringLiteral("%1 vs %2").arg(bands[0]).arg(bands[1])));
#endif
#endif
    }

    void terminalBrightensFaintText()
    {
        // 256-color 235 (#262626) on the dark background is moved until readable; readable colors stay.
        const QColor bg(0x17, 0x17, 0x19), faint(0x26, 0x26, 0x26), fine(0xd4, 0xd4, 0xd8);
        const QColor out = TerminalWidget::readable(faint, bg, 3.0);
        QVERIFY2(out.lightness() > 90, qPrintable(out.name()));
        QCOMPARE(TerminalWidget::readable(fine, bg, 3.0), fine);
        // On a light background faint text gets darker.
        QVERIFY(TerminalWidget::readable(QColor(0xee, 0xee, 0xee), QColor(0xfb, 0xfb, 0xfc), 3.0).lightness() < 0xee);
    }

    void terminalKeysSelectionAndScrollback()
    {
        // The terminal's own keys: editing the line, ⌃C, exit and restart with Return, selecting and
        // copying with the mouse, ⌘V, ⌘K, scrolling back, and window titles / folders the shell reports.
        useTestShell();
        TerminalWidget term;
        term.resize(1200, 300); // wide: CI prompts carry long paths, and a wrapped line is two lines
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));
        term.start(m_tmp.path());
        QTRY_VERIFY2_WITH_TIMEOUT(term.isReady(), qPrintable(term.screenText()), 15000);
        term.activateWindow();
        term.setFocus();
        QTRY_VERIFY(term.hasFocus());
        auto *pty = term.findChild<Pty *>();
        QVERIFY(pty);
        auto lines = [&] { return term.screenText().split(QLatin1Char('\n')); };
        auto hasLine = [&](const QString &l) { return lines().contains(l); };

        // Backspace edits the line; Return runs it.
        QTest::keyClicks(&term, "echo abcX");
        QTest::keyClick(&term, Qt::Key_Backspace);
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY2(hasLine(QStringLiteral("abc")), qPrintable(term.screenText()));

#ifndef Q_OS_WIN
        // A half-typed line holds back the browser's cd; erased again, the cd goes through.
        QTest::keyClicks(&term, "ab");
        term.followFolder(p("Alpha"));
        QTest::qWait(900); // two polls
        QVERIFY(!term.isAt(p("Alpha")));
        QTest::keyClick(&term, Qt::Key_Backspace);
        QTest::keyClick(&term, Qt::Key_Backspace);
        QTRY_VERIFY2(term.isAt(p("Alpha")), qPrintable(term.screenText()));

        // A command taken back from the history is a line too: the cd must wait for it.
        QTRY_VERIFY(!term.isBusy());
        QTest::keyClick(&term, Qt::Key_Up);
        QTest::qWait(300); // the shell puts "echo abc" back on the line
        term.followFolder(p("Beta"));
        QTest::qWait(900);
        QVERIFY2(!term.isAt(p("Beta")), "a line recalled with ↑ must not be replaced by the browser's cd");
        QTest::keyClick(&term, Qt::Key_C, kTerminalMods); // ⌃C: drop the line, then the cd goes through
        QTRY_VERIFY(term.isAt(p("Beta")));

        // ⌃C interrupts a running program.
        QTest::keyClicks(&term, "sleep 30");
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY(term.isBusy());
        QTest::keyClick(&term, Qt::Key_C, kTerminalMods);
        QTRY_VERIFY(!term.isBusy());
#endif

#ifndef Q_OS_WIN // ConPTY repaints the screen from its own buffer: text fed in below wouldn't stay
        // Titles and folders reported by programs (OSC 2 / OSC 7).
        QSignalSpy titled(&term, &TerminalWidget::titleChanged);
        pty->dataReceived(QByteArray("\x1b]2;My Title\x07"));
        QCOMPARE(term.tabTitle(), QStringLiteral("My Title"));
        QVERIFY(!titled.isEmpty());
        QSignalSpy moved(&term, &TerminalWidget::cwdChanged);
        pty->dataReceived("\x1b]7;" + QUrl::fromLocalFile(p("감마")).toEncoded() + "\x07");
        QCOMPARE(term.shellCwd(), p("감마"));
        QCOMPARE(moved.size(), 1);
        pty->dataReceived(QByteArray("\x1b]2;\x07"));
        QCOMPARE(term.tabTitle(), QStringLiteral("감마")); // no title: the folder's name

        // ⌘K clears the screen and the history.
        QTest::keyClick(&term, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(!hasLine(QStringLiteral("abc")));
        QTest::qWait(300); // the shell redraws its prompt

        // Selecting with the mouse and ⌘C copy (Ctrl+C copies too, but only with a selection).
        pty->dataReceived(QByteArray("\x1b[2J\x1b[HCOPYME rest-of-line\r\n"));
        QTest::mouseDClick(&term, Qt::LeftButton, {}, QPoint(3, 3));
        QTest::keyClick(&term, Qt::Key_C, Qt::ControlModifier);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("COPYME"));
        QTest::mousePress(&term, Qt::LeftButton, {}, QPoint(3, 3));
        QTest::mouseMove(&term, QPoint(term.width() - 4, 3));
        QTest::mouseRelease(&term, Qt::LeftButton, {}, QPoint(term.width() - 4, 3));
        QTest::keyClick(&term, Qt::Key_C, Qt::ControlModifier);
        QTRY_COMPARE(QGuiApplication::clipboard()->text().trimmed(), QStringLiteral("COPYME rest-of-line"));

        // Scrolling back: the wheel shows earlier lines (a double-click there selects from them).
        QByteArray many;
        for (int i = 1; i <= 120; ++i)
            many += QStringLiteral("L%1\r\n").arg(i, 3, 10, QLatin1Char('0')).toLatin1();
        pty->dataReceived(many);
        QWheelEvent up(QPointF(50, 50), term.mapToGlobal(QPointF(50, 50)), QPoint(), QPoint(0, 120 * 100), Qt::NoButton,
                       Qt::NoModifier, Qt::NoScrollPhase, false);
        auto wordAtTop = [&] {
            QGuiApplication::clipboard()->setText(QStringLiteral("-"));
            QTest::mouseDClick(&term, Qt::LeftButton, {}, QPoint(3, 3));
            QTest::keyClick(&term, Qt::Key_C, Qt::ControlModifier);
            return QGuiApplication::clipboard()->text();
        };
        QApplication::sendEvent(&term, &up);
        QCOMPARE(wordAtTop(), QStringLiteral("COPYME")); // the top of the history
        QTest::keyClick(&term, Qt::Key_PageDown, Qt::ShiftModifier); // a page further down
        const QString paged = wordAtTop();
        QVERIFY2(paged.startsWith(QLatin1Char('L')) && paged.mid(1).toInt() > 1, qPrintable(paged));
        QTest::keyClick(&term, Qt::Key_PageUp, Qt::ShiftModifier);
        QCOMPARE(wordAtTop(), QStringLiteral("COPYME"));
        QTest::keyClick(&term, Qt::Key_K, Qt::ControlModifier); // the history goes too
        QTest::qWait(300);
        QApplication::sendEvent(&term, &up);
        QVERIFY(!wordAtTop().startsWith(QLatin1Char('L')));
        QVERIFY(wordAtTop() != QStringLiteral("COPYME"));

        // ⌘V pastes; Ctrl+Backspace (⌘⌫) clears the half-typed line; dropped text is typed in.
        QGuiApplication::clipboard()->setText(QStringLiteral("echo pasted_ok"));
        QTest::keyClick(&term, Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(term.screenText().contains(QStringLiteral("echo pasted_ok")));
        QTest::keyClick(&term, Qt::Key_Backspace, Qt::ControlModifier);
        QTRY_VERIFY(!term.screenText().contains(QStringLiteral("echo pasted_ok")));
        QMimeData text;
        text.setText(QStringLiteral("echo dropped_ok"));
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &text, Qt::LeftButton, {});
        QApplication::sendEvent(&term, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &text, Qt::LeftButton, {});
        QApplication::sendEvent(&term, &drop);
        QVERIFY(drop.isAccepted());
        QTRY_VERIFY2(term.screenText().contains(QStringLiteral("echo dropped_ok")), qPrintable(term.screenText()));
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY2(hasLine(QStringLiteral("dropped_ok")), qPrintable(term.screenText()));

        // The shell exits: a note, and Return starts a new one in the same folder.
        QTest::keyClicks(&term, "exit");
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY(!term.isRunning());
        QTRY_VERIFY(term.screenText().contains(QStringLiteral("프로세스가 종료됨")));
        QTest::keyClicks(&term, "x"); // ignored while nothing runs
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY(term.isRunning());
        QTRY_VERIFY_WITH_TIMEOUT(term.isReady(), 15000);
#endif
        Settings::instance()->setValue(Settings::TermShell, QString());
    }

#ifndef Q_OS_WIN // the shells' line editing and cat -v below are Unix's
    // Once the cursor moves inside the line (←, Home, ⌃A, …) what is typed or erased no longer adds
    // up: the browser's cd, which clears the line first, must wait — or it wiped a half-typed command.
    void terminalCursorKeysKeepTheLine()
    {
        useTestShell();
        TerminalWidget term;
        term.resize(1200, 300);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));
        term.start(m_tmp.path());
        QTRY_VERIFY2_WITH_TIMEOUT(term.isReady(), qPrintable(term.screenText()), 15000);
        term.setFocus();
        QTRY_VERIFY(term.hasFocus());
        QTest::keyClicks(&term, "echo ab");
        QTest::keyClick(&term, Qt::Key_Home);
        QTest::keyClick(&term, Qt::Key_Backspace); // at the line's start: erases nothing
        QTest::keyClick(&term, Qt::Key_Backspace);
        QTest::keyClick(&term, Qt::Key_Backspace);
        QTest::keyClick(&term, Qt::Key_Backspace);
        QTest::keyClick(&term, Qt::Key_Backspace);
        QTest::keyClick(&term, Qt::Key_Backspace);
        QTest::keyClick(&term, Qt::Key_Backspace); // seven: as many as were typed
        term.followFolder(p("Alpha"));
        QTest::qWait(900); // two polls
        QVERIFY2(!term.isAt(p("Alpha")), qPrintable(term.screenText()));
        QVERIFY(term.screenText().contains(QStringLiteral("echo ab")));
        QTest::keyClick(&term, Qt::Key_C, kTerminalMods); // ⌃C: the line is dropped, the cd goes through
        QTRY_VERIFY(term.isAt(p("Alpha")));
        // The same for ⌃A / ⌃E (line start / end, the shell's own keys).
        QTRY_VERIFY(!term.isBusy());
        QTest::keyClicks(&term, "x");
        QTest::keyClick(&term, Qt::Key_A, kTerminalMods);
        QTest::keyClick(&term, Qt::Key_Backspace);
        term.followFolder(p("Beta"));
        QTest::qWait(900);
        QVERIFY(!term.isAt(p("Beta")));
        QTest::keyClick(&term, Qt::Key_C, kTerminalMods);
        QTRY_VERIFY(term.isAt(p("Beta")));
    }

    // Full-screen programs (less, vim): the wheel becomes arrow keys, or wheel buttons once they track
    // the mouse; clicks and double-clicks go to them too. cat -v writes down what it receives.
    void terminalFullScreenProgramsGetWheelAndMouse()
    {
        useTestShell();
        TerminalWidget term;
        term.resize(1200, 300);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));
        term.start(m_tmp.path());
        QTRY_VERIFY2_WITH_TIMEOUT(term.isReady(), qPrintable(term.screenText()), 15000);
        term.setFocus();
        auto *pty = term.findChild<Pty *>();
        QVERIFY(pty);
        QFile::remove(p("keys.txt"));
        QTest::keyClicks(&term, "cat -v > keys.txt");
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY(term.isBusy());
        pty->dataReceived(QByteArray("\x1b[?1049h")); // the program switches to the alternate screen
        QWheelEvent up(QPointF(50, 50), term.mapToGlobal(QPointF(50, 50)), QPoint(), QPoint(0, 120), Qt::NoButton,
                       Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&term, &up); // one notch: three lines
        pty->dataReceived(QByteArray("\x1b[?1000h")); // ... and asks for the mouse
        // A double click as the system delivers it: press, release, double-click (for the second press), release.
        QTest::mouseClick(&term, Qt::LeftButton, {}, QPoint(20, 20));
        QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(20, 20), term.mapToGlobal(QPointF(20, 20)), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&term, &dbl);
        QTest::mouseRelease(&term, Qt::LeftButton, {}, QPoint(20, 20));
        QApplication::sendEvent(&term, &up);
        QTest::keyClick(&term, Qt::Key_Return);
        QTest::keyClick(&term, Qt::Key_D, kTerminalMods); // end of input: cat exits
        QTRY_VERIFY(!term.isBusy());
        QFile f(p("keys.txt"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray got = f.readAll();
        const int arrows = got.count("^[[A") + got.count("^[OA");
        QVERIFY2(arrows == 3, got.constData());
        QVERIFY2(got.count("^[[M ") == 2, got.constData()); // presses: the click's and the double-click's second
        QVERIFY2(got.count("^[[M#") == 2 + 3, got.constData()); // their releases, and the wheel's
        QVERIFY2(got.count("^[[M`") == 3, got.constData()); // wheel up, as buttons now (64 + 32)
        pty->dataReceived(QByteArray("\x1b[?1000l\x1b[?1049l"));
    }

    // A program that takes the mouse gets a click on release, with its modifiers: Shift-click (range selection)
    // and ⌘-click as Ctrl-click (the report has no ⌘; a toggle in Ginote's list). A drag selects text and sends nothing.
    void terminalMouseClicksCarryModifiers()
    {
        useTestShell();
        TerminalWidget term;
        term.resize(1200, 300);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));
        term.start(m_tmp.path());
        QTRY_VERIFY2_WITH_TIMEOUT(term.isReady(), qPrintable(term.screenText()), 15000);
        term.setFocus();
        auto *pty = term.findChild<Pty *>();
        QVERIFY(pty);
        QFile::remove(p("mouse.txt"));
        QTest::keyClicks(&term, "cat -v > mouse.txt");
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY(term.isBusy());
        pty->dataReceived(QByteArray("\x1b[?1000h"));
        QTest::mouseClick(&term, Qt::LeftButton, Qt::ShiftModifier, QPoint(20, 20));
        QTest::mouseClick(&term, Qt::LeftButton, Qt::ControlModifier, QPoint(20, 20)); // ⌘ on macOS, Ctrl elsewhere
        QTest::mousePress(&term, Qt::LeftButton, {}, QPoint(20, 20));
        QTest::mouseMove(&term, QPoint(300, 20));
        QTest::mouseRelease(&term, Qt::LeftButton, {}, QPoint(300, 20));
        pty->dataReceived(QByteArray("\x1b[?1000l"));
        QTest::keyClick(&term, Qt::Key_Return);
        QTest::keyClick(&term, Qt::Key_D, kTerminalMods);
        QTRY_VERIFY(!term.isBusy());
        QFile f(p("mouse.txt"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray got = f.readAll();
        QVERIFY2(got.count("^[[M") == 4, got.constData()); // two clicks, nothing for the drag
        QVERIFY2(got.count("^[[M$") == 1 && got.count("^[[M'") == 1, got.constData()); // Shift: press 0|4, release 3|4
        QVERIFY2(got.count("^[[M0") == 1 && got.count("^[[M3") == 1, got.constData()); // Ctrl: press 0|16, release 3|16
    }

    // The composing text stays where the cursor was last shown while a program repaints with the cursor hidden
    // (Bubble Tea does, every frame). A click commits it once, even when the IME commits it again afterwards.
    void terminalComposingTextFollowsTheShownCursor()
    {
        useTestShell();
        TerminalWidget term;
        term.resize(1200, 300);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));
        term.start(m_tmp.path());
        QTRY_VERIFY2_WITH_TIMEOUT(term.isReady(), qPrintable(term.screenText()), 15000);
        term.setFocus();
        auto *pty = term.findChild<Pty *>();
        QVERIFY(pty);
        QFile::remove(p("ime.txt"));
        QTest::keyClicks(&term, "cat > ime.txt");
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY(term.isBusy());
        QWidget *w = &term;
        pty->dataReceived(QByteArray("\x1b[5;10H"));
        const QRect at = w->inputMethodQuery(Qt::ImCursorRectangle).toRect();
        pty->dataReceived(QByteArray("\x1b[?25l\x1b[1;1Hxx\x1b[9;1H")); // a repaint elsewhere, cursor hidden
        QCOMPARE(w->inputMethodQuery(Qt::ImCursorRectangle).toRect(), at);
        pty->dataReceived(QByteArray("\x1b[5;12H\x1b[?25h")); // shown again at its new place
        QCOMPARE(w->inputMethodQuery(Qt::ImCursorRectangle).toRect().top(), at.top());
        QVERIFY(w->inputMethodQuery(Qt::ImCursorRectangle).toRect().left() > at.left());
        QInputMethodEvent composing(QStringLiteral("가"), {});
        QApplication::sendEvent(&term, &composing);
        QTest::mouseClick(&term, Qt::LeftButton, {}, QPoint(20, 20));
        QInputMethodEvent late;
        late.setCommitString(QStringLiteral("가"));
        QApplication::sendEvent(&term, &late);
        QTest::keyClick(&term, Qt::Key_Return);
        QTest::keyClick(&term, Qt::Key_D, kTerminalMods);
        QTRY_VERIFY(!term.isBusy());
        QFile f(p("ime.txt"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("가\n"));
    }
#endif

    // Two terminals whose programs set their titles in pieces, interleaved: each keeps its own.
    void terminalTitlesStayPerTab()
    {
        useTestShell();
        TerminalWidget a, b;
        a.start(m_tmp.path());
        b.start(m_tmp.path());
        // Past the first prompt: the shells' own folder reports (OSC 7) are in by then.
        QTRY_VERIFY_WITH_TIMEOUT(a.isReady() && b.isReady(), 15000);
        auto *pa = a.findChild<Pty *>(), *pb = b.findChild<Pty *>();
        QVERIFY(pa && pb);
        pa->dataReceived(QByteArray("\x1b]2;Fir"));
        pb->dataReceived(QByteArray("\x1b]2;Sec"));
        pa->dataReceived(QByteArray("st\x07"));
        pb->dataReceived(QByteArray("ond\x07"));
        QCOMPARE(a.tabTitle(), QStringLiteral("First"));
        QCOMPARE(b.tabTitle(), QStringLiteral("Second"));
        pa->dataReceived("\x1b]7;" + QUrl::fromLocalFile(p("Al")).toEncoded());
        pb->dataReceived("\x1b]7;" + QUrl::fromLocalFile(p("Be")).toEncoded());
        pa->dataReceived(QByteArray("pha\x07"));
        pb->dataReceived(QByteArray("ta\x07"));
        QCOMPARE(a.shellCwd(), p("Alpha"));
        QCOMPARE(b.shellCwd(), p("Beta"));
    }

#ifndef Q_OS_WIN // ConPTY repaints the screen from its own buffer: text fed in below wouldn't stay
    // Scrolled back into a full history (5000 lines kept), new output doesn't move what is shown.
    void terminalFullHistoryStaysStill()
    {
        useTestShell();
        TerminalWidget term;
        term.resize(1200, 300);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));
        term.start(m_tmp.path());
        QTRY_VERIFY(term.isRunning());
        auto *pty = term.findChild<Pty *>();
        QByteArray many;
        for (int i = 0; i < 5200; ++i)
            many += QStringLiteral("L%1\r\n").arg(i, 5, 10, QLatin1Char('0')).toLatin1();
        pty->dataReceived(many);
        auto wordAtTop = [&] {
            QGuiApplication::clipboard()->setText(QStringLiteral("-"));
            QTest::mouseDClick(&term, Qt::LeftButton, {}, QPoint(3, 3));
            QTest::keyClick(&term, Qt::Key_C, Qt::ControlModifier);
            return QGuiApplication::clipboard()->text();
        };
        QWheelEvent up(QPointF(50, 50), term.mapToGlobal(QPointF(50, 50)), QPoint(), QPoint(0, 120 * 10), Qt::NoButton,
                       Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&term, &up);
        const QString shown = wordAtTop();
        QVERIFY2(shown.startsWith(QLatin1Char('L')), qPrintable(shown));
        pty->dataReceived(QByteArray("more\r\nand more\r\n"));
        QCOMPARE(wordAtTop(), shown);
    }

#endif

    // Right click without a program that takes the mouse: 복사 (only with a selection), 붙여넣기, 지우기.
    void terminalContextMenu()
    {
        useTestShell();
        TerminalWidget term;
        term.resize(1000, 300);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));
        term.start(m_tmp.path());
        QTRY_VERIFY2_WITH_TIMEOUT(term.isReady(), qPrintable(term.screenText()), 15000);
        auto menu = [&](const QString &item) {
            QStringList enabled;
            QTimer::singleShot(50, this, [&] {
                if (auto *m = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                    for (QAction *a : m->actions())
                        if (a->isEnabled() && !a->isSeparator())
                            enabled << a->text();
                    for (QAction *a : m->actions())
                        if (a->text() == item)
                            a->trigger();
                    m->close();
                }
            });
            QTest::mousePress(&term, Qt::RightButton, {}, QPoint(30, 30));
            QTest::mouseRelease(&term, Qt::RightButton, {}, QPoint(30, 30));
            return enabled;
        };
        QGuiApplication::clipboard()->setText(QStringLiteral("echo menu_paste_ok"));
        QCOMPARE(menu(QStringLiteral("붙여넣기")), (QStringList{QStringLiteral("붙여넣기"), QStringLiteral("지우기")}));
        QTRY_VERIFY2(term.screenText().contains(QStringLiteral("echo menu_paste_ok")), qPrintable(term.screenText()));
        QTest::keyClick(&term, Qt::Key_Return);
        QTRY_VERIFY(term.screenText().split(QLatin1Char('\n')).contains(QStringLiteral("menu_paste_ok")));
        QTest::mouseDClick(&term, Qt::LeftButton, {}, QPoint(3, 3));
        QVERIFY(menu(QString()).contains(QStringLiteral("복사")));
        QGuiApplication::clipboard()->clear();
        menu(QStringLiteral("복사"));
        QVERIFY(!QGuiApplication::clipboard()->text().isEmpty());
#ifndef Q_OS_WIN // ConPTY keeps its own copy of the screen
        menu(QStringLiteral("지우기"));
        QTRY_VERIFY(!term.screenText().contains(QStringLiteral("menu_paste_ok")));
#endif
    }

    // The app-wide undo history keeps the last 100 actions; a new action empties redo.
    void undoHistoryKeepsTheLast100()
    {
        App *app = App::instance();
        while (app->canUndo())
            app->takeUndo();
        while (app->canRedo())
            app->takeRedo();
        const QString dir = p("Alpha");
        for (int i = 0; i < 105; ++i)
            app->recordDone({QStringLiteral("기록 %1").arg(i), {{Step::Mkdir, QString(), dir + QStringLiteral("/n%1").arg(i)}}});
        QCOMPARE(app->undoLabel(), QStringLiteral("기록 104"));
        int kept = 0;
        QString oldest;
        while (app->canUndo()) {
            const UndoRecord r = app->takeUndo();
            app->recordUndone(r);
            oldest = r.label;
            ++kept;
        }
        QCOMPARE(kept, 100);
        QCOMPARE(oldest, QStringLiteral("기록 5"));
        QCOMPARE(app->redoLabel(), QStringLiteral("기록 5"));
        app->recordDone({QStringLiteral("새 작업"), {{Step::Mkdir, QString(), dir + QStringLiteral("/new")}}});
        QVERIFY(!app->canRedo());
        app->recordDone({}); // nothing done: no record
        QCOMPARE(app->undoLabel(), QStringLiteral("새 작업"));
        app->takeUndo();
        RecentFolders::instance()->clear();
    }

    void sidebarFavoritesInsertAtPosition()
    {
        Sidebar *sb = m_sidebar;
        const QStringList before = Sidebar::favorites();
        sb->insertFavorites({p("Alpha"), p("README.md")}, 1);
        QStringList favs = Sidebar::favorites();
        QCOMPARE(favs.mid(1, 2), (QStringList{p("Alpha"), p("README.md")}));
        QCOMPARE(favs.size(), before.size() + 2);
        sb->insertFavorites({p("Alpha")}, 0); // existing entry moves instead of duplicating
        favs = Sidebar::favorites();
        QCOMPARE(favs.first(), p("Alpha"));
        QCOMPARE(favs.count(p("Alpha")), 1);
        Sidebar::setFavorites(before);
    }

    // macOS Full Disk Access guide: asked once at start unless granted or "다시 묻지 않기"; from the menu
    // (force) always, saying so when it is already granted. (설정 열기 opens System Settings: not pressed.)
    void fullDiskAccessGuide()
    {
        const auto restore = qScopeGuard([] { Settings::instance()->remove(Settings::DontAskFullDisk); });
        auto shown = [this]() -> QDialog * {
            for (QDialog *d : m_win->findChildren<QDialog *>())
                if (d->isVisible())
                    return d;
            return nullptr;
        };
#ifdef Q_OS_MACOS
        if (Permissions::hasFullDiskAccess()) { // the terminal running the tests may have it
            QTimer::singleShot(100, this, [] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    box->accept();
            });
            Permissions::showDialog(m_win, true); // "already granted"
            Permissions::showDialog(m_win, false);
            QVERIFY(!shown());
            QSKIP("Full Disk Access is granted here: the guide itself isn't shown");
        }
        Permissions::showDialog(m_win, false);
        QPointer<QDialog> dlg = shown();
        QVERIFY(dlg);
        QVERIFY(showsText(dlg, QStringLiteral("폴더 접근을 한 번에 허용해 주세요")));
        auto *dontAsk = dlg->findChild<QCheckBox *>();
        QVERIFY(dontAsk);
        dontAsk->setChecked(true);
        buttonNamed(dlg, QStringLiteral("나중에"))->click();
        QTRY_VERIFY(!dlg);
        QVERIFY(Settings::instance()->flag(Settings::DontAskFullDisk));
        Permissions::showDialog(m_win, false); // not again at start
        QVERIFY(!shown());
        Permissions::showDialog(m_win, true); // from the menu: yes, without the checkbox
        dlg = shown();
        QVERIFY(dlg && !dlg->findChild<QCheckBox *>());
        buttonNamed(dlg, QStringLiteral("나중에"))->click();
        QTRY_VERIFY(!dlg);
#else
        QVERIFY(Permissions::hasFullDiskAccess()); // nothing to grant elsewhere
        Permissions::showDialog(m_win, false);
        QVERIFY(!shown());
        QTimer::singleShot(100, this, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->accept();
        });
        Permissions::showDialog(m_win, true);
#endif
    }

    // Section titles fold and unfold with one click (and Return), show ⌄ / › beside them, and stay as
    // they were left (the next start too).
    void sidebarSectionsFoldWithOneClick()
    {
        Sidebar *sb = m_sidebar;
        const auto restore = qScopeGuard([sb] {
            QSettings().remove(QStringLiteral("sidebar/folded"));
            emit Theme::instance()->changed(); // rebuilds: all open again
            sb->clearFocus();
        });
        emit Theme::instance()->changed();
        auto section = [sb](const QString &title) -> QTreeWidgetItem * {
            for (int i = 0; i < sb->topLevelItemCount(); ++i)
                if (sb->topLevelItem(i)->text(0) == title)
                    return sb->topLevelItem(i);
            return nullptr;
        };
        QTreeWidgetItem *fav = section(QStringLiteral("즐겨찾기"));
        QVERIFY(fav && fav->isExpanded() && fav->childCount() > 0);
        QVERIFY(section(QStringLiteral("최근 폴더")) && section(QStringLiteral("위치")));
        const QImage openShot = sb->viewport()->grab().toImage();
        QTest::mouseClick(sb->viewport(), Qt::LeftButton, {}, sb->visualItemRect(fav).center());
        QVERIFY(!fav->isExpanded());
        QVERIFY(!sb->visualItemRect(fav->child(0)).isValid()); // its rows are gone
        QCOMPARE(QSettings().value(QStringLiteral("sidebar/folded")).toStringList(), QStringList{QStringLiteral("favorites")});
        QVERIFY(sb->viewport()->grab().toImage() != openShot); // › instead of ⌄ (and the rows)
        // Kept through a rebuild (and so the next start).
        emit Theme::instance()->changed();
        fav = section(QStringLiteral("즐겨찾기"));
        QVERIFY(!fav->isExpanded());
        QVERIFY(section(QStringLiteral("위치"))->isExpanded());
        // One more click opens it; a double click is two clicks, so it ends where it was.
        QTest::mouseClick(sb->viewport(), Qt::LeftButton, {}, sb->visualItemRect(fav).center());
        QVERIFY(fav->isExpanded());
        { // as the system delivers a double click: press, release, double click, release
            const QPoint at = sb->visualItemRect(fav).center();
            QTest::mouseClick(sb->viewport(), Qt::LeftButton, {}, at);
            QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(at), sb->viewport()->mapToGlobal(QPointF(at)), Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(sb->viewport(), &dbl);
            QTest::mouseRelease(sb->viewport(), Qt::LeftButton, {}, at);
        }
        QVERIFY(fav->isExpanded());
        QVERIFY(QSettings().value(QStringLiteral("sidebar/folded")).toStringList().isEmpty());
        // From the keyboard: Return on a title folds it, a click on a place still opens it.
        sb->setFocus();
        sb->setCurrentItem(section(QStringLiteral("위치")), 0, QItemSelectionModel::NoUpdate);
        QTest::keyClick(sb, Qt::Key_Return);
        QVERIFY(!section(QStringLiteral("위치"))->isExpanded());
        QTest::keyClick(sb, Qt::Key_Return);
        QVERIFY(section(QStringLiteral("위치"))->isExpanded());
        QSignalSpy opened(sb, &Sidebar::placeActivated);
        QTest::mouseClick(sb->viewport(), Qt::LeftButton, {}, sb->visualItemRect(section(QStringLiteral("위치"))->child(0)).center());
        QCOMPARE(opened.size(), 1);
    }

    // "최근 폴더": a section between the favorites and the volumes (its title alone while empty);
    // a click opens one, the menu adds it to the favorites or takes it off the list.
    void sidebarShowsRecentFolders()
    {
        Sidebar *sb = m_sidebar;
        RecentFolders *r = RecentFolders::instance();
        const QStringList favorites = Sidebar::favorites();
        const auto restore = qScopeGuard([r, favorites] {
            r->clear();
            Sidebar::setFavorites(favorites);
            Settings::instance()->remove(Settings::RecentFoldersCount);
            emit Theme::instance()->changed();
        });
        r->clear();
        auto titles = [sb] {
            QStringList out;
            for (int i = 0; i < sb->topLevelItemCount(); ++i)
                out << sb->topLevelItem(i)->text(0);
            return out;
        };
        auto recentPaths = [sb] {
            QStringList out;
            for (int i = 0; i < sb->topLevelItemCount(); ++i)
                if (sb->topLevelItem(i)->text(0) == QStringLiteral("최근 폴더"))
                    for (int j = 0; j < sb->topLevelItem(i)->childCount(); ++j)
                        out << QDir::fromNativeSeparators(sb->topLevelItem(i)->child(j)->toolTip(0));
            return out;
        };
        auto rowOf = [sb](const QString &path) -> QTreeWidgetItem * {
            for (int i = 0; i < sb->topLevelItemCount(); ++i)
                if (sb->topLevelItem(i)->text(0) == QStringLiteral("최근 폴더"))
                    for (int j = 0; j < sb->topLevelItem(i)->childCount(); ++j)
                        if (sb->topLevelItem(i)->child(j)->toolTip(0) == QDir::toNativeSeparators(path))
                            return sb->topLevelItem(i)->child(j);
            return nullptr;
        };
        const QStringList withRecent{QStringLiteral("즐겨찾기"), QStringLiteral("최근 폴더"), QStringLiteral("위치")};
        QTRY_COMPARE(titles(), withRecent); // empty: the title alone
        QVERIFY(recentPaths().isEmpty());
        r->note(p("Alpha"));
        r->note(p("Beta"));
        QTRY_COMPARE(recentPaths(), (QStringList{p("Beta"), p("Alpha")}));
        QCOMPARE(rowOf(p("Beta"))->text(0), QStringLiteral("Beta"));

        // The open folder is marked there too; a click asks for it.
        sb->setCurrentPath(p("Alpha"));
        QVERIFY(rowOf(p("Alpha"))->isSelected());
        QSignalSpy activated(sb, &Sidebar::placeActivated);
        QTest::mouseClick(sb->viewport(), Qt::LeftButton, {}, sb->visualItemRect(rowOf(p("Beta"))).center());
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.first().first().toString(), p("Beta"));

        // Files dropped on a recent folder go into it.
        QSignalSpy dropped(sb, &Sidebar::dropRequested);
        {
            QMimeData md;
            md.setUrls({QUrl::fromLocalFile(p("README.md"))});
            const QPoint at = sb->visualItemRect(rowOf(p("Alpha"))).center();
            QDragEnterEvent enter(at, Qt::CopyAction | Qt::MoveAction, &md, Qt::LeftButton, {});
            QApplication::sendEvent(sb->viewport(), &enter);
            QDragMoveEvent move(at, Qt::CopyAction | Qt::MoveAction, &md, Qt::LeftButton, {});
            QApplication::sendEvent(sb->viewport(), &move);
            QVERIFY(move.isAccepted());
            QDropEvent drop(QPointF(at), Qt::CopyAction | Qt::MoveAction, &md, Qt::LeftButton, {});
            QApplication::sendEvent(sb->viewport(), &drop);
        }
        QTRY_COMPARE(dropped.size(), 1);
        QCOMPARE(dropped.first().at(1).toString(), p("Alpha"));

        // Context menu: 즐겨찾기에 추가, 최근 폴더에서 제거, 최근 폴더 모두 지우기.
        auto menuOn = [&](const QString &path, const QString &item) {
            QStringList offered;
            QTimer::singleShot(50, this, [&] {
                if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                    for (QAction *a : menu->actions()) {
                        offered << a->text();
                        if (a->text() == item)
                            a->trigger();
                    }
                    menu->close();
                }
            });
            const QPoint at = sb->visualItemRect(rowOf(path)).center();
            QContextMenuEvent e(QContextMenuEvent::Mouse, at, sb->viewport()->mapToGlobal(at));
            QApplication::sendEvent(sb->viewport(), &e);
            return offered;
        };
        Sidebar::setFavorites({});
        emit Theme::instance()->changed(); // rebuilds the sidebar (it doesn't follow the setting itself)
        QCOMPARE(menuOn(p("Beta"), QStringLiteral("즐겨찾기에 추가")),
                 (QStringList{QStringLiteral("새로운 탭에서 열기"), QStringLiteral("즐겨찾기에 추가"), QStringLiteral("최근 폴더에서 제거"),
                              QStringLiteral("최근 폴더 모두 지우기")}));
        QCOMPARE(Sidebar::favorites(), QStringList{p("Beta")});
        QTRY_VERIFY(rowOf(p("Beta")));
        QVERIFY(!menuOn(p("Beta"), QString()).contains(QStringLiteral("즐겨찾기에 추가"))); // already one
        menuOn(p("Beta"), QStringLiteral("최근 폴더에서 제거"));
        QTRY_COMPARE(recentPaths(), QStringList{p("Alpha")});
        QSignalSpy newTab(sb, &Sidebar::openInNewTab);
        menuOn(p("Alpha"), QStringLiteral("새로운 탭에서 열기"));
        QCOMPARE(newTab.size(), 1);
        r->note(p("Beta"));
        QTRY_COMPARE(recentPaths().size(), 2);
        menuOn(p("Alpha"), QStringLiteral("최근 폴더 모두 지우기"));
        QTRY_VERIFY(recentPaths().isEmpty());
        QCOMPARE(titles(), withRecent); // the title stays

        // sidebar.recent_folders: how many; 0 hides the section.
        for (const char *d : {"Alpha", "Beta", "감마"})
            r->note(p(QString::fromUtf8(d)));
        Settings::instance()->setValue(Settings::RecentFoldersCount, 2);
        QTRY_COMPARE(recentPaths(), (QStringList{p("감마"), p("Beta")}));
        Settings::instance()->setValue(Settings::RecentFoldersCount, 0);
        QTRY_COMPARE(titles(), (QStringList{QStringLiteral("즐겨찾기"), QStringLiteral("위치")}));
    }

    // 설정 → 선택 항목 메뉴: 추가 opens the command editor (name, letter, command, terminal), 편집 changes
    // a command, 위로 / 아래로 reorder, 삭제 removes a command of one's own, 기본값으로 (asked first) resets.
    void selectionMenuPageEditsCommands()
    {
        Settings *st = Settings::instance();
        const auto restore = qScopeGuard([st] { st->remove(Settings::SelectionCommands); });
        st->remove(Settings::SelectionCommands);
        QPointer<QWidget> dlg = settingsAt(QStringLiteral("선택 항목 메뉴"));
        QVERIFY(dlg);
        auto *list = dlg->findChild<QListWidget *>(QStringLiteral("commandList"));
        QVERIFY(list);
        const int builtins = list->count();
        auto labels = [st] {
            QStringList out;
            for (const QVariant &c : st->value(Settings::SelectionCommands).toList())
                out << c.toMap().value(QStringLiteral("label")).toString();
            return out;
        };
        auto editor = [] () -> QDialog * {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (w->isVisible() && w->objectName() == QLatin1String("commandEdit"))
                    return qobject_cast<QDialog *>(w);
            return nullptr;
        };
        dlg->findChild<QPushButton *>(QStringLiteral("commandAdd"))->click();
        QPointer<QDialog> ed;
        QTRY_VERIFY((ed = editor()));
        auto *label = ed->findChild<QLineEdit *>(QStringLiteral("commandLabel"));
        auto *letter = ed->findChild<QLineEdit *>(QStringLiteral("commandKey"));
        auto *text = ed->findChild<QPlainTextEdit *>(QStringLiteral("commandText"));
        auto *terminal = ed->findChild<QCheckBox *>(QStringLiteral("commandTerminal"));
        QPushButton *ok = buttonNamed(ed, QStringLiteral("확인"));
        QVERIFY(label && letter && text && terminal && ok);
        label->clear();
        QVERIFY(!ok->isEnabled()); // a name and a command are needed
        label->setText(QStringLiteral("내 명령"));
        QVERIFY(ok->isEnabled());
        QTest::keyClicks(letter, QStringLiteral("q!")); // one letter or digit
        QCOMPARE(letter->text(), QStringLiteral("q"));
        text->setPlainText(QStringLiteral("echo {names}"));
        terminal->setChecked(false);
        ok->click();
        QTRY_VERIFY(!ed);
        QCOMPARE(list->count(), builtins + 1);
        const QVariantMap mine = st->value(Settings::SelectionCommands).toList().last().toMap();
        QCOMPARE(mine.value(QStringLiteral("label")).toString(), QStringLiteral("내 명령"));
        QCOMPARE(mine.value(QStringLiteral("key")).toString(), QStringLiteral("Q"));
        QCOMPARE(mine.value(QStringLiteral("terminal")).toBool(), false);
        QCOMPARE(mine.value(QStringLiteral("command")).toString(), QStringLiteral("echo {names}"));
        QCOMPARE(list->currentRow(), builtins);
        QVERIFY(list->currentItem()->text().contains(QStringLiteral("(Q)")));
        QVERIFY(list->currentItem()->text().contains(QStringLiteral("터미널 없이")));

        // 편집, then 위로 / 아래로.
        dlg->findChild<QPushButton *>(QStringLiteral("commandEdit"))->click();
        QTRY_VERIFY((ed = editor()));
        ed->findChild<QLineEdit *>(QStringLiteral("commandLabel"))->setText(QStringLiteral("고친 명령"));
        buttonNamed(ed, QStringLiteral("확인"))->click();
        QTRY_VERIFY(!ed);
        QCOMPARE(labels().last(), QStringLiteral("고친 명령"));
        auto *down = dlg->findChild<QPushButton *>(QStringLiteral("commandDown"));
        auto *up = dlg->findChild<QPushButton *>(QStringLiteral("commandUp"));
        QVERIFY(!down->isEnabled()); // already last
        up->click();
        QCOMPARE(labels().at(builtins - 1), QStringLiteral("고친 명령"));
        QCOMPARE(list->currentRow(), builtins - 1);
        down->click();
        QCOMPARE(labels().last(), QStringLiteral("고친 명령"));
        // 취소 in the editor changes nothing.
        dlg->findChild<QPushButton *>(QStringLiteral("commandEdit"))->click();
        QTRY_VERIFY((ed = editor()));
        ed->findChild<QLineEdit *>(QStringLiteral("commandLabel"))->setText(QStringLiteral("버릴 이름"));
        buttonNamed(ed, QStringLiteral("취소"))->click();
        QTRY_VERIFY(!ed);
        QCOMPARE(labels().last(), QStringLiteral("고친 명령"));

        // 기본값으로 asks; 취소 keeps it, 예 resets.
        auto *defaults = dlg->findChild<QPushButton *>(QStringLiteral("commandDefaults"));
        answerNextQuestion(QMessageBox::Cancel);
        defaults->click();
        QCOMPARE(list->count(), builtins + 1);
        answerNextQuestion(QMessageBox::Yes);
        defaults->click();
        QCOMPARE(list->count(), builtins);
        // 삭제 on one's own command (a built-in's 삭제 is off: selectionMenuKeepsBuiltins).
        st->setValue(Settings::SelectionCommands, st->value(Settings::SelectionCommands).toList() << mine);
        QTRY_COMPARE(list->count(), builtins + 1); // the page follows config.toml
        list->setCurrentRow(builtins);
        dlg->findChild<QPushButton *>(QStringLiteral("commandRemove"))->click();
        QCOMPARE(list->count(), builtins);
        dlg->close();
    }

    // 설정 → 단축키 초기화 / 전부 제거 and 모양 및 색상 → 색 초기화 ask first; 취소 leaves everything.
    void settingsResetsAskFirst()
    {
        Shortcuts *sc = Shortcuts::instance();
        Settings *st = Settings::instance();
        const auto restore = qScopeGuard([sc, st] {
            sc->resetAll();
            st->remove(Settings::FolderColor);
        });
        sc->assign(QStringLiteral("열기"), QKeySequence(QStringLiteral("F6")));
        QPointer<QWidget> dlg = settingsAt(QStringLiteral("단축키"));
        QVERIFY(dlg);
        auto *reset = dlg->findChild<QPushButton *>(QStringLiteral("shortcutsReset"));
        auto *clear = dlg->findChild<QPushButton *>(QStringLiteral("shortcutsClear"));
        QVERIFY(reset && clear);
        answerNextQuestion(QMessageBox::Cancel);
        reset->click();
        QVERIFY(sc->isCustom(QStringLiteral("열기")));
        answerNextQuestion(QMessageBox::Yes);
        reset->click();
        QVERIFY(!sc->isCustom(QStringLiteral("열기")));
        answerNextQuestion(QMessageBox::Yes);
        clear->click();
        QVERIFY(sc->keys(QStringLiteral("열기")).isEmpty());
        sc->resetAll();

        st->setValue(Settings::FolderColor, QStringLiteral("#123456"));
        dlg = settingsAt(Gifiles::tr("모양 및 색상"));
        auto *colorsReset = dlg->findChild<QPushButton *>(QStringLiteral("colorsReset"));
        QVERIFY(colorsReset);
        answerNextQuestion(QMessageBox::Cancel);
        colorsReset->click();
        QCOMPARE(st->value(Settings::FolderColor).toString(), QStringLiteral("#123456"));
        answerNextQuestion(QMessageBox::Yes);
        colorsReset->click();
        QVERIFY(st->value(Settings::FolderColor).toString().isEmpty());
        // A choice (theme) on the 모양 page writes config.toml at once.
        QComboBox *theme = nullptr;
        for (QComboBox *c : dlg->findChildren<QComboBox *>())
            if (c->findData(QStringLiteral("dark")) >= 0)
                theme = c;
        QVERIFY(theme);
        const int was = theme->currentIndex();
        theme->setCurrentIndex((was + 1) % theme->count());
        QCOMPARE(st->value(Settings::ThemeMode).toString(), theme->currentData().toString());
        theme->setCurrentIndex(was);
        dlg->close();
    }

    void fileNamesColoredByExtension()
    {
        Settings *st = Settings::instance();
        st->remove(Settings::FileColors);
        st->remove(Settings::FolderColor);
        auto color = [](const char *name) { return Theme::fileColor(QString::fromLatin1(name)); };
        auto shown = [](const char *hex) { return Theme::colors().dark ? QColor(hex) : Theme::lightModeColor(QColor(hex)); };
        // Built-in groups: one color per group, different groups differ.
        QCOMPARE(color("a.png"), color("b.jpg"));
        QCOMPARE(color("a.cpp"), color("a.h"));
        QCOMPARE(color("a.zip"), color("a.tar.gz"));
        QCOMPARE(color("a.zip"), color("a.iso")); // disk images are containers
        QCOMPARE(color("a.exe"), color("a.apk")); // installers go with the programs
        QCOMPARE(color("a.exe"), color("a.dmg"));
        QCOMPARE(color("a.sql"), color("a.py"));
        QVERIFY(color("a.png") != color("a.mp3"));
        QVERIFY(color("a.png") != color("a.mp4")); // pictures and video: two greens
        QVERIFY(color("a.exe") != color("a.bat"));
        QCOMPARE(color("x.JPG"), color("y.jpg"));
        QVERIFY(!color("Makefile").isValid()); // no extension: plain
        QVERIFY(!color("a.qqq").isValid());    // not listed: plain
        QVERIFY(!Theme::folderColor().isValid()); // folders: the normal text color unless set

        // config.toml decides: the first group naming an extension wins, the dot and case don't matter.
        st->setValue(Settings::FileColors, QVariantList{
            QVariantMap{{QStringLiteral("extensions"), QStringLiteral("qqq, .ZIP")}, {QStringLiteral("color"), QStringLiteral("#123456")}},
            QVariantMap{{QStringLiteral("extensions"), QStringLiteral("zip")}, {QStringLiteral("color"), QStringLiteral("#FF0000")}}});
        QCOMPARE(color("a.qqq"), shown("#123456"));
        QCOMPARE(color("a.zip"), shown("#123456"));
        QVERIFY(!color("a.png").isValid());
        st->setValue(Settings::FolderColor, QStringLiteral("#00FF00"));
        QCOMPARE(Theme::folderColor(), shown("#00FF00"));
        // Light mode: the same hue, darker and stronger.
        const QColor light = Theme::lightModeColor(QColor(0xDC, 0x88, 0xDC));
        QVERIFY(light.lightness() < 110);
        QVERIFY(qAbs(light.hslHue() - QColor(0xDC, 0x88, 0xDC).hslHue()) <= 2);
        // ...and every hue the same weight on white: yellow no lighter than blue (HSL scaling left it pale).
        auto luma = [](const QColor &c) { return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF(); };
        QVERIFY(qAbs(luma(Theme::lightModeColor(QColor(0xF8, 0xDF, 0x44))) - luma(Theme::lightModeColor(QColor(0x3E, 0x5F, 0xEA)))) < 0.06);
        // A wrong value is reported.
        QVERIFY(!Settings::check(QStringLiteral("[file_colors]\ngroups = [ { extensions = \"a\", color = \"red\" } ]\n")).isEmpty());
        QVERIFY(!Settings::check(QStringLiteral("[file_colors]\nfolder = \"#12345\"\n")).isEmpty());
        QVERIFY(Settings::check(QStringLiteral("[file_colors]\nfolder = \"#123456\"\ngroups = []\n")).isEmpty());
        QVERIFY(Settings::check(QStringLiteral("[file_colors]\nfolder = \"\"\n")).isEmpty());
        QVERIFY(Settings::check(QStringLiteral("[appearance]\nnative_title_bar = true\n")).isEmpty()); // removed option: ignored

        // The 색상 page: one row per group; a new row is saved once it has extensions.
        st->remove(Settings::FileColors);
        // Unchanged colors are written commented out, so later default colors still reach this file.
        QVERIFY(st->render().contains(QStringLiteral("\n# groups = [\n")));
        QVERIFY(Settings::check(st->render()).isEmpty());
        SettingsDialog::showSingleton(m_win, Gifiles::tr("모양 및 색상"));
        QPointer<QWidget> dlg;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<SettingsDialog *>(w) && w->isVisible())
                dlg = w;
        QVERIFY(dlg);
        auto *body = dlg->findChild<QWidget *>(QStringLiteral("colorRows"));
        QVERIFY(body);
        // 초기화 sits small above the colors: at the bottom center it was taken for the OK button.
        auto *colorsReset = dlg->findChild<QPushButton *>(QStringLiteral("colorsReset"));
        QVERIFY(colorsReset && colorsReset->isVisible());
        QVERIFY(colorsReset->mapTo(dlg, QPoint()).y() < body->mapTo(dlg, QPoint()).y());
        auto rows = [body] { return body->findChildren<QWidget *>(QStringLiteral("colorRow"), Qt::FindDirectChildrenOnly); };
        QCOMPARE(rows().size(), Settings::defaultFileColors().size());
        dlg->findChild<QPushButton *>(QStringLiteral("colorAdd"))->click();
        QCOMPARE(rows().size(), Settings::defaultFileColors().size() + 1);
        QCOMPARE(st->value(Settings::FileColors).toList().size(), Settings::defaultFileColors().size()); // empty: not yet
        QWidget *row = rows().last();
        auto *ext = row->findChild<QLineEdit *>(QStringLiteral("colorExt"));
        auto *hex = row->findChild<QLineEdit *>(QStringLiteral("colorHex"));
        hex->setText(QStringLiteral("abcdef"));
        emit hex->editingFinished();
        QCOMPARE(hex->text(), QStringLiteral("#ABCDEF"));
        ext->setText(QStringLiteral("qqq"));
        emit ext->editingFinished();
        QCOMPARE(st->value(Settings::FileColors).toList().last().toMap().value(QStringLiteral("color")).toString(), QStringLiteral("#ABCDEF"));
        QCOMPARE(color("a.qqq"), shown("#ABCDEF"));
        row->findChild<QPushButton *>(QStringLiteral("colorRemove"))->click();
        QVERIFY(!color("a.qqq").isValid());
        QCOMPARE(rows().size(), Settings::defaultFileColors().size());
        // The picker previews each color in the lists at once; 취소 puts the stored one back, 확인 keeps it.
        {
            QWidget *first = rows().first();
            const QColor before = color("a.exe");
            first->findChild<QPushButton *>(QStringLiteral("colorSwatch"))->click();
            QColorDialog *picker = nullptr;
            QTRY_VERIFY((picker = dlg->window()->findChild<QColorDialog *>(QStringLiteral("colorPicker"))) != nullptr);
            picker->setCurrentColor(QColor(0x12, 0x34, 0x56));
            QCOMPARE(color("a.exe"), shown("#123456"));
            picker->reject();
            QCOMPARE(color("a.exe"), before);
            first->findChild<QPushButton *>(QStringLiteral("colorSwatch"))->click();
            QTRY_VERIFY((picker = dlg->window()->findChild<QColorDialog *>(QStringLiteral("colorPicker"))) != nullptr);
            picker->setCurrentColor(QColor(0x65, 0x43, 0x21));
            picker->accept();
            QCOMPARE(first->findChild<QLineEdit *>(QStringLiteral("colorHex"))->text(), QStringLiteral("#654321"));
            QCOMPARE(color("a.exe"), shown("#654321"));
            st->remove(Settings::FileColors);
        }
        auto *folder = dlg->findChild<QLineEdit *>(QStringLiteral("folderColorHex"));
        QCOMPARE(folder->text(), QStringLiteral("#00FF00"));
        st->remove(Settings::FolderColor); // config.toml changed elsewhere: the page follows
        QCOMPARE(folder->text(), QString());
        folder->setText(QStringLiteral("#cd6a51"));
        emit folder->editingFinished();
        QCOMPARE(st->value(Settings::FolderColor).toString(), QStringLiteral("#CD6A51"));
        folder->clear(); // empty: back to the text color
        emit folder->editingFinished();
        QVERIFY(!Theme::folderColor().isValid());
        dlg->close();
        st->remove(Settings::FileColors);
    }

    void selectionMenuKeepsBuiltins()
    {
        // The three built-in commands can be changed but not removed: one left out of the file
        // comes back; a key letter and terminal = false survive the round trip.
        Settings *s = Settings::instance();
        QVariantMap mine{{QStringLiteral("label"), QStringLiteral("내 스크립트")}, {QStringLiteral("key"), QStringLiteral("R")},
                         {QStringLiteral("terminal"), false}, {QStringLiteral("command"), QStringLiteral("echo {files}")}};
        s->setValue(Settings::SelectionCommands, QVariantList{mine});
        const QVariantList all = s->value(Settings::SelectionCommands).toList();
        QCOMPARE(all.size(), 4);
        QCOMPARE(all.first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("내 스크립트"));
        QVERIFY(!s->selectionCommand(QStringLiteral("ai")).isEmpty());
        QFile cfg(Settings::configPath());
        QVERIFY(cfg.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(cfg.readAll());
        QVERIFY(text.contains(QStringLiteral("{ label = \"내 스크립트\", key = \"R\", terminal = false, command = \"echo {files}\" }")));
        QVERIFY(Settings::check(text).isEmpty());
        QVERIFY(!Settings::check(QStringLiteral("[selection_menu]\ncommands = [ { label = \"a\", key = \"ab\", command = \"x\" } ]\n")).isEmpty());
        QVERIFY(!Settings::check(QStringLiteral("[selection_menu]\ncommands = [ { id = \"nope\", label = \"a\", command = \"x\" } ]\n")).isEmpty());
        QVERIFY(Settings::check(QStringLiteral("[ai]\ntool = \"claude\"\n")).isEmpty()); // the old AI settings are dropped quietly
        // In the settings window a built-in one can't be deleted.
        SettingsDialog::showSingleton(m_win, QStringLiteral("선택 항목 메뉴"));
        QPointer<QWidget> dlg;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<SettingsDialog *>(w) && w->isVisible())
                dlg = w;
        QVERIFY(dlg);
        auto *list = dlg->findChild<QListWidget *>(QStringLiteral("commandList"));
        auto *remove = dlg->findChild<QPushButton *>(QStringLiteral("commandRemove"));
        QVERIFY(list && remove);
        list->setCurrentRow(0);
        QVERIFY(remove->isEnabled());
        list->setCurrentRow(1);
        QVERIFY(!remove->isEnabled());
        dlg->close();
        QTRY_VERIFY(!dlg);
        s->remove(Settings::SelectionCommands);
    }

    void renamedShortcutIdsKeepTheirKeys()
    {
        // 훑어보기 became 퀵 뷰어: a key saved under the old name still applies to the new one.
        const QStringList renamedProblems = Settings::check(QStringLiteral("[shortcuts]\n\"훑어보기\" = [\"F9\"]\n\"컨텍스트 메뉴 새 탭에서 열기\" = [\"T\"]\n"));
        QVERIFY2(renamedProblems.isEmpty(), qPrintable(renamedProblems.join(QLatin1Char('|'))));
        QVERIFY(!Settings::check(QStringLiteral("[shortcuts]\n\"없는 항목\" = [\"F9\"]\n")).isEmpty());
        const QString path = Settings::configPath();
        // From the settings in memory, not the file: a save can still be pending there (Windows).
        const QString original = Settings::instance()->render();
        QVERIFY(original.contains(QStringLiteral("[shortcuts]")));
        auto writeConfig = [&](const QString &text) {
            QVERIFY(replaceFile(path, text.toUtf8())); // like an editor: replace the file
        };
        writeConfig(QString(original).replace(QStringLiteral("[shortcuts]"), QStringLiteral("[shortcuts]\n\"훑어보기\" = [\"F9\"]")));
        QTRY_COMPARE(Shortcuts::instance()->keys(QStringLiteral("퀵 뷰어")), QList<QKeySequence>{QKeySequence(QStringLiteral("F9"))});
        QVERIFY(Settings::instance()->problems().isEmpty());
        writeConfig(original);
        QTRY_VERIFY(!Shortcuts::instance()->isCustom(QStringLiteral("퀵 뷰어")));
    }

    void openWithListsApps()
    {
        const QList<OpenWith::App> apps = OpenWith::appsFor(p("README.md"));
#ifdef Q_OS_MACOS
        QVERIFY2(!apps.isEmpty(), "NSWorkspace returned no apps for .md");
        QVERIFY(apps.first().isDefault);
        QStringList names;
        for (const auto &a : apps)
            names << a.name;
        qInfo().noquote() << "apps for .md:" << names.mid(0, 6).join(QStringLiteral(", "));
        OpenWith::showDialog(m_win, {p("README.md")});
        QDialog *dlg = nullptr;
        QTRY_VERIFY((dlg = m_win->findChild<QDialog *>(QString(), Qt::FindDirectChildrenOnly)) && dlg->isVisible());
        shot("10-open-with", dlg);
        dlg->reject();
#else
        Q_UNUSED(apps);
#endif
    }

    void openWithRemembersTheChosenApp()
    {
        // "항상 이 앱으로 열기": the remembered app comes first in 다음으로 열기, even if the system
        // doesn't list it, and is preselected with the switch on.
        write(QStringLiteral("doc.qqq"), "x");
        QVERIFY(QDir().mkpath(p("Fake Editor.app")));
        const QString key = QStringLiteral("open_with/qqq");
        const auto restore = qScopeGuard([this, key] {
            Settings::instance()->remove(key);
            QFile::remove(p("doc.qqq"));
            QDir(p("Fake Editor.app")).removeRecursively();
        });
        QVERIFY(!OpenWith::hasRememberedApp(p("doc.qqq")));
        Settings::instance()->setValue(key, p("Fake Editor.app"));
        QVERIFY(OpenWith::hasRememberedApp(p("doc.qqq")));
        QVERIFY(OpenWith::hasRememberedApp(p("other.QQQ"))); // per extension, any case
        const QList<OpenWith::App> apps = OpenWith::appsFor(p("doc.qqq"));
        QVERIFY(!apps.isEmpty());
        QCOMPARE(apps.first().id, p("Fake Editor.app"));
        QCOMPARE(apps.first().name, QStringLiteral("Fake Editor"));
#ifndef Q_OS_WIN // Windows shows its own dialog
        OpenWith::showDialog(m_win, {p("doc.qqq")});
        QPointer<QDialog> dlg;
        auto shownDialog = [this] {
            for (QDialog *d : m_win->findChildren<QDialog *>(QString(), Qt::FindDirectChildrenOnly))
                if (d->isVisible() && d->windowTitle() == Gifiles::tr("다음으로 열기"))
                    return d;
            return static_cast<QDialog *>(nullptr);
        };
        QTRY_VERIFY((dlg = shownDialog()));
        auto *list = dlg->findChild<QListWidget *>();
        QVERIFY(list && list->currentItem());
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), p("Fake Editor.app"));
        auto *always = dlg->findChild<QCheckBox *>();
        QVERIFY(always && always->isChecked() && always->text().contains(QStringLiteral(".qqq")));
        buttonNamed(dlg, Gifiles::tr("취소"))->click(); // nothing opens, nothing changes
        QTRY_VERIFY(!dlg);
        QVERIFY(OpenWith::hasRememberedApp(p("doc.qqq")));
#endif
    }

    void shortcutsCanBeRebound()
    {
        Shortcuts *sc = Shortcuts::instance();
        SettingsDialog::showSingleton(m_win);
        QPointer<QWidget> dlg;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<SettingsDialog *>(w) && w->isVisible())
                dlg = w;
        QVERIFY(dlg);
        auto *rebind = dlg->findChild<QPushButton *>(QStringLiteral("rebind:열기"));
        QVERIFY(rebind);
        // 재지정 opens a small window that shows the key pressed; Return and Esc are keys too
        // (they can be shortcuts): only 확인 / 취소 close it.
        auto capture = [&] {
            rebind->click();
            QDialog *cap = nullptr;
            for (int i = 0; i < 100 && !cap; ++i, QTest::qWait(10))
                for (QWidget *w : QApplication::topLevelWidgets())
                    if (w->objectName() == QLatin1String("keyCapture") && w->isVisible())
                        cap = qobject_cast<QDialog *>(w);
            return QPointer<QDialog>(cap);
        };
        QPointer<QDialog> cap = capture();
        QVERIFY(cap);
        QTest::keyClick(cap, Qt::Key_Escape);
        QVERIFY(cap && cap->isVisible());
        QCOMPARE(cap->findChild<QLabel *>(QStringLiteral("keyCaptureText"))->text(), QKeySequence(Qt::Key_Escape).toString(QKeySequence::NativeText));
        QTest::keyClick(cap, Qt::Key_D, Qt::ControlModifier); // ⌘D belongs to 복제: said so, in red
        QVERIFY(cap->findChild<QLabel *>(QStringLiteral("warning"))->text().contains(QStringLiteral("복제")));
        cap->findChild<QPushButton *>(QStringLiteral("keyCaptureCancel"))->click();
        QTRY_VERIFY(!cap);
        QCOMPARE(sc->keys(QStringLiteral("열기")), sc->defaults(QStringLiteral("열기"))); // unchanged
        QCOMPARE(sc->keys(QStringLiteral("복제")).size(), 1);
        cap = capture();
        QVERIFY(cap);
        QTest::keyClick(cap, Qt::Key_J, Qt::ControlModifier);
        QTest::keyClick(cap, Qt::Key_Return); // replaces ⌘J in the box, …
        QTest::keyClick(cap, Qt::Key_J, Qt::ControlModifier); // … and the last key pressed counts
        QVERIFY(cap->isVisible());
        cap->findChild<QPushButton *>(QStringLiteral("keyCaptureOk"))->click();
        QTRY_VERIFY(!cap);
        QCOMPARE(sc->keys(QStringLiteral("열기")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+J"))});
        QVERIFY(sc->isCustom(QStringLiteral("열기")));
        dlg->close();
        QTRY_VERIFY(!dlg);
        // Saved in config.toml (that the window's key then works: smoke reboundKeyWorksInTheWindow).
        auto saved = [] {
            QFile cfg(Settings::configPath());
            return cfg.open(QIODevice::ReadOnly) && QString::fromUtf8(cfg.readAll()).contains(QStringLiteral("\"열기\" = [\"Ctrl+J\"]"));
        };
        QTRY_VERIFY(saved());
        // A key taken by another action moves over; resetting gives the built-in keys back.
        QCOMPARE(sc->assign(QStringLiteral("열기"), QKeySequence(QStringLiteral("Ctrl+D"))), QStringList{QStringLiteral("복제")});
        QVERIFY(sc->keys(QStringLiteral("복제")).isEmpty());
        sc->reset(QStringLiteral("복제"));
        QCOMPARE(sc->keys(QStringLiteral("복제")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+D"))});
        QVERIFY(sc->keys(QStringLiteral("열기")).isEmpty()); // ⌘D went back to 복제
        sc->reset(QStringLiteral("열기"));
        QCOMPARE(sc->keys(QStringLiteral("열기")), sc->defaults(QStringLiteral("열기"))); // ⌘↓, ⌘O
        QVERIFY(!sc->isCustom(QStringLiteral("열기")));
        // The page's 전부 제거 / 초기화 act on every shortcut.
        sc->clearAll();
        QVERIFY(sc->keys(QStringLiteral("열기")).isEmpty() && sc->keys(QStringLiteral("복제")).isEmpty());
        sc->resetAll();
        QCOMPARE(sc->keys(QStringLiteral("열기")), sc->defaults(QStringLiteral("열기")));
        QVERIFY(!sc->isCustom(QStringLiteral("복제")));
    }

    // The debug log (only started on the development machine): one file a day, a week kept; keys with
    // their modifiers, typing only counted (never its text), message boxes, Qt warnings. Last: once
    // started it stays on for the process.
    void debugLogRecordsKeysNotText()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto touch = [&](const QString &name) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        const QString old = QStringLiteral("gifiles-%1.log").arg(QDate::currentDate().addDays(-8).toString(Qt::ISODate));
        const QString recent = QStringLiteral("gifiles-%1.log").arg(QDate::currentDate().addDays(-2).toString(Qt::ISODate));
        touch(old);
        touch(recent);
        touch(QStringLiteral("notes.txt"));
        QVERIFY(!Log::enabled());
        Log::write("test", QStringLiteral("not yet")); // off: nothing happens
        Log::start(dir.path());
        QVERIFY(Log::enabled());
        QVERIFY(!QFileInfo::exists(dir.filePath(old)));   // older than a week: gone
        QVERIFY(QFileInfo::exists(dir.filePath(recent))); // kept
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("notes.txt"))));

        QWidget host;
        auto *edit = new QLineEdit(&host);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        edit->setFocus();
        QTRY_VERIFY(edit->hasFocus());
        // As the system delivers keys: a ShortcutOverride first, each key with its own timestamp
        // (QTest's carry none, and the log tells one key from its repeats by the timestamp).
        ulong stamp = 1000;
        auto press = [&](int key, Qt::KeyboardModifiers mods, const QString &text) {
            QKeyEvent over(QEvent::ShortcutOverride, key, mods, text);
            over.setTimestamp(++stamp);
            QApplication::sendEvent(edit, &over);
            QKeyEvent down(QEvent::KeyPress, key, mods, text);
            down.setTimestamp(stamp);
            QApplication::sendEvent(edit, &down);
        };
        for (const QChar c : QStringLiteral("secret"))
            press(Qt::Key_A + (c.unicode() - 'a'), Qt::NoModifier, QString(c));
        QCOMPARE(edit->text(), QStringLiteral("secret"));
        press(Qt::Key_A, Qt::ControlModifier, QString()); // a command key: recorded
        QCOMPARE(Log::recentKey(), QKeySequence(Qt::CTRL | Qt::Key_A).toString(QKeySequence::NativeText));
        QTest::qWait(1700); // the typing count is flushed after a pause
        auto *box = new QMessageBox(QMessageBox::Information, QStringLiteral("제목"), QStringLiteral("본문"), QMessageBox::Ok, &host);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();
        box->close();
        qWarning("a warning for the log");
        Log::write("test", QStringLiteral("한글 줄"));

        QFile f(dir.filePath(QStringLiteral("gifiles-%1.log").arg(QDate::currentDate().toString(Qt::ISODate))));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        QVERIFY2(!text.contains(QStringLiteral("secret")), qPrintable(text)); // what was typed never is
        QVERIFY2(text.contains(QStringLiteral("[typing] 6 characters (QLineEdit)")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("[key] ") + Log::recentKey()), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral(" | 본문")) && text.contains(QStringLiteral("[dialog] ")), qPrintable(text)); // macOS boxes have no title
        QVERIFY(text.contains(QStringLiteral("[qt-warning] a warning for the log")));
        QVERIFY(text.contains(QStringLiteral("[test] 한글 줄")));
        QVERIFY(!text.contains(QStringLiteral("not yet")));
        QVERIFY(QRegularExpression(QStringLiteral("^\\d\\d:\\d\\d:\\d\\d\\.\\d{3} \\[")).match(text).hasMatch());
    }

};

QTEST_MAIN(Widgets)
#include "unit_widgets.moc"
