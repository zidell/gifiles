// End-to-end smoke test: opens a real MainWindow on a scratch folder and drives it with
// synthetic key presses the way a user would. Screenshots go to $OUT (if set).
//
//   QT_QPA_PLATFORM=offscreen OUT=/tmp/shots ./build/gifiles_smoke

#include "App.h"
#include "BrowserTab.h"
#include "MainWindow.h"
#include "FileProxy.h"
#include "ItemDelegate.h"
#include "OpenWith.h"
#include "PathBar.h"
#include "Preview.h"
#include "Pty.h"
#include "Settings.h"
#include <QColorDialog>
#include "Shortcuts.h"
#include "Sidebar.h"
#include "SystemPreview.h"
#include "TerminalWidget.h"
#include "Theme.h"
#include "Util.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QAction>
#include <QDialog>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QClipboard>
#include <QMenu>
#include <QPushButton>
#include <QSaveFile>
#include <QToolButton>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>
#include <QPlainTextEdit>
#include <QHeaderView>
#include <QTreeView>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QStyleOptionMenuItem>
#include <QStyleFactory>
#include <QScopeGuard>
#include <QSlider>
#include <QScrollBar>
#include <QCheckBox>
#include <QPdfWriter>
#include <QWheelEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>
#include <QFileSystemModel>

// Shortcuts that use the physical Control key on the Mac differ per platform (see MainWindow.cpp).
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifiers kTerminalMods = Qt::MetaModifier;                         // ⌃`
constexpr Qt::KeyboardModifiers kNewFolderWithMods = Qt::ControlModifier | Qt::MetaModifier; // ⌃⌘N
#else
constexpr Qt::KeyboardModifiers kTerminalMods = Qt::ControlModifier;
constexpr Qt::KeyboardModifiers kNewFolderWithMods = Qt::ControlModifier | Qt::AltModifier;
#endif

class Smoke : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QTemporaryDir m_cfg; // config.toml lives here, not in the user's own
    MainWindow *m_win = nullptr;

    QString p(const QString &rel) const { return QDir(m_tmp.path()).filePath(rel); }
    QString np(const QString &rel) const { return QDir::toNativeSeparators(p(rel)); } // as the shell shows it
    BrowserTab *tab() const { return m_win->tab(); }
    QWidget *focus() const { return QApplication::focusWidget() ? QApplication::focusWidget() : m_win; }
    void key(int k, Qt::KeyboardModifiers m = Qt::NoModifier) { QTest::keyClick(focus(), Qt::Key(k), m); }
    void type(const QString &s)
    {
        // QTest can only synthesize ASCII keys; insert other text the way an input method would.
        if (auto *le = qobject_cast<QLineEdit *>(focus()); le && s.toLatin1() != s.toUtf8())
            le->insert(s);
        else
            QTest::keyClicks(focus(), s);
    }
    void shot(const QString &name, QWidget *w = nullptr)
    {
        const QString out = qEnvironmentVariable("OUT");
        if (out.isEmpty())
            return;
        QDir().mkpath(out);
        QTest::qWait(150);
        (w ? w : m_win)->grab().save(QDir(out).filePath(name + QStringLiteral(".png")));
    }
    // Presses ⌘↓ and, if it shows a menu (files are selected), returns its first item's text
    // after choosing it (accept) or closing the menu.
    // viaAction: run the 열기 action instead of pressing its key (Windows offscreen loses the
    // keyboard focus after a dialog closed; what's tested then is the menu, not the key).
    QString openKey(bool accept, std::function<void(QMenu *)> onMenu = {}, bool viaAction = false)
    {
        // Offscreen, closing a popup leaves no active window (macOS gives it back to the window).
        QTest::qWait(50);
        // Windows offscreen: right after a dialog closed, one request isn't always enough.
        for (int i = 0; i < 5 && QApplication::activeWindow() != m_win; ++i) {
            m_win->activateWindow();
            QTest::qWaitForWindowActive(m_win, 500);
        }
        if (QApplication::activeWindow() != m_win)
            return QStringLiteral("window not active");
        tab()->focusView();
        struct State { QString first; bool done = false; };
        auto st = std::make_shared<State>();
        auto grab = std::make_shared<std::function<void()>>();
        *grab = [st, accept, onMenu, w = std::weak_ptr<std::function<void()>>(grab)] {
            if (st->done)
                return;
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                st->done = true;
                QAction *a = menu->actions().value(0);
                st->first = a ? a->text() : QString();
                if (onMenu) {
                    onMenu(menu);
                    if (menu->isVisible())
                        menu->close();
                    return;
                }
                if (accept && a)
                    a->trigger(); // the menu owns its actions: trigger before it goes away
                menu->close();
            } else if (auto g = w.lock()) {
                QTimer::singleShot(20, *g);
            }
        };
        QTimer::singleShot(20, *grab);
        if (viaAction) {
            if (auto *a = m_win->findChild<QAction *>(QStringLiteral("열기")))
                a->trigger();
        } else {
            key(Qt::Key_Down, Qt::ControlModifier);
        }
        st->done = true; // a folder opens without a menu
        grab.reset();
        QTest::qWait(50);
        m_win->activateWindow(); // for the keys that follow
        (void)QTest::qWaitForWindowActive(m_win);
        tab()->focusView();
        return st->first;
    }

    void write(const QString &rel, const QByteArray &data)
    {
        QFile f(p(rel));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }

    // QAbstractItemView::state() is protected. A press while the list runs an expand/collapse
    // animation (AnimatingState) is not treated as a click.
    static bool viewIdle(QAbstractItemView *v)
    {
        struct Peek : QAbstractItemView {
            static bool idle(QAbstractItemView *v) { return (v->*(&Peek::state))() == NoState; }
        };
        return Peek::idle(v);
    }

    // Whether the file model lists `name` in folder `dir` (asked without creating the entry).
    bool modelLists(const QString &dir, const QString &name) const
    {
        auto *proxy = static_cast<FileProxy *>(tab()->view()->model());
        const QModelIndex parent = proxy->indexForPath(dir);
        for (int r = 0; r < proxy->rowCount(parent); ++r)
            if (proxy->index(r, 0, parent).data().toString() == name)
                return true;
        return false;
    }

    // A fresh folder of the test's own in the scratch folder, already listed by the model. A folder
    // entered right after it was made can lose its row to a late listing of its parent.
    bool makeFixture(const QString &name)
    {
        QDir(p(name)).removeRecursively();
        if (!QDir().mkpath(p(name)))
            return false;
        tab()->navigate(m_tmp.path());
        return QTest::qWaitFor([&] { return modelLists(m_tmp.path(), name); }, 10000);
    }

    // Names in the current folder, in the order the view shows them.
    QStringList shownNames() const
    {
        QAbstractItemView *v = tab()->view();
        QStringList out;
        for (int r = 0; r < v->model()->rowCount(v->rootIndex()); ++r)
            out << v->model()->index(r, 0, v->rootIndex()).data().toString();
        return out;
    }

    // Menus and message boxes run their own event loop: set this up before the key or click that
    // opens one. `fn` gets the first popup or modal window that shows up (within 5 s).
    void onNextPopup(std::function<void(QWidget *)> fn)
    {
        auto *timer = new QTimer(this);
        auto tries = std::make_shared<int>(0);
        connect(timer, &QTimer::timeout, this, [timer, fn, tries] {
            QWidget *w = QApplication::activePopupWidget();
            if (!w)
                w = QApplication::activeModalWidget();
            if ((w && w->isVisible()) || ++*tries > 250) {
                timer->stop();
                timer->deleteLater();
                if (w && w->isVisible())
                    fn(w);
            }
        });
        timer->start(20);
    }

    static QPushButton *buttonNamed(QWidget *w, const QString &text)
    {
        for (QPushButton *b : w->findChildren<QPushButton *>())
            if (b->text().remove(QLatin1Char('&')) == text)
                return b;
        return nullptr;
    }

    // Replaces a file the way an editor does. On Windows the app (or a virus scanner) can hold it for a
    // moment right after its own save, and the replace fails: try again, as editors do.
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

    static bool showsText(QWidget *w, const QString &text)
    {
        for (QLabel *l : w->findChildren<QLabel *>())
            if (l->isVisibleTo(w) && l->text().contains(text))
                return true;
        return false;
    }

    QString readFile(const QString &rel) const
    {
        QFile f(p(rel));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

    static void useTestShell()
    {
#if defined(Q_OS_MACOS)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/sh")); // no user rc files
#elif !defined(Q_OS_WIN)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/bash"));
#endif
    }

private slots:
    void initTestCase()
    {
        PreviewWidget::chooseMediaBackend(); // as main() does
        // Own settings namespace, reset every run (view mode, favorites, session).
        qputenv("GIFILES_CONFIG_DIR", m_cfg.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("gifiles-smoke"));
        QSettings().clear();
        Theme::instance()->install();
        QVERIFY(m_tmp.isValid());
        for (const char *d : {"Alpha", "Beta", "Beta/inner", "감마"})
            QDir().mkpath(p(QString::fromUtf8(d)));
        write(QStringLiteral("notes.txt"), "hello\nworld\n\tindented\n");
        write(QStringLiteral("README.md"), "# Title\n\nSome *markdown*.\n");
        for (int i : {1, 2, 10})
            write(QStringLiteral("file%1.go").arg(i), "package main\n");
        write(QStringLiteral("Beta/inner/deep.txt"), "deep\n");
        QImage img(320, 200, QImage::Format_RGB32);
        QPainter g(&img);
        QLinearGradient grad(0, 0, 320, 200);
        grad.setColorAt(0, QColor(255, 120, 40));
        grad.setColorAt(1, QColor(40, 90, 255));
        g.fillRect(img.rect(), grad);
        g.end();
        QVERIFY(img.save(p(QStringLiteral("photo.png"))));

        m_win = App::instance()->newWindow({m_tmp.path()});
        m_win->resize(1100, 680);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_win->activateWindow();
        tab()->focusView();
        QTRY_COMPARE(tab()->itemCount(), 9);
    }

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

    // Every test starts from the scratch folder in list view with the file list focused.
    void init()
    {
        // An error box left open by a failed test would keep focus and fail every later test too.
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible())
                box->close();
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
        tab()->navigate(m_tmp.path());
        tab()->setMode(BrowserTab::List, true); // folders remember their view; reset this one
        tab()->focusView();
    }

    void appIconResourceRenders()
    {
        const QIcon icon(QStringLiteral(":/gifiles.svg"));
        QVERIFY(!icon.isNull());
        for (int size : {16, 32, 64, 256})
            QVERIFY(!icon.pixmap(size, size).isNull());
    }

    void cleanup()
    {
        // Failed terminal tests must not leave a typed line, running program or open
        // panel for the next test, which shares this window.
        auto *bar = m_win->findChild<QTabBar *>(QStringLiteral("termTabs"));
        while (bar && bar->count() > 0)
            emit bar->tabCloseRequested(bar->currentIndex());
        QTRY_VERIFY(m_win->findChildren<TerminalWidget *>().isEmpty());
        Settings::instance()->setValue(Settings::TermShell, QString());
    }

    void terminalResizeKeepsCursorInRange()
    {
        // A wrapped line's final row contains the cursor when the whole line is
        // too tall for the resized screen. libvterm 0.3.3 used to abort here.
        VTerm *vt = vterm_new(3, 80);
        VTermScreen *screen = vterm_obtain_screen(vt);
        vterm_screen_enable_reflow(screen, true);
        vterm_screen_reset(screen, 1);
        const QByteArray text(200, 'x');
        vterm_input_write(vt, text.constData(), size_t(text.size()));
        vterm_set_size(vt, 2, 10);
        const QByteArray next("\r\nOK");
        vterm_input_write(vt, next.constData(), size_t(next.size()));
        VTermPos cursor;
        vterm_state_get_cursorpos(vterm_obtain_state(vt), &cursor);
        VTermScreenCell cell;
        const bool read = vterm_screen_get_cell(screen, VTermPos{cursor.row, 1}, &cell);
        vterm_free(vt);
        QVERIFY(cursor.row >= 0 && cursor.row < 2);
        QCOMPARE(cursor.col, 2);
        QVERIFY(read);
        QCOMPARE(cell.chars[0], uint32_t('K'));
    }

    void shortcutsAreUnique()
    {
        // Qt silently ignores a key sequence bound to two actions (or twice to one).
        QHash<QString, QString> seen;
        for (QAction *a : m_win->actions())
            for (const QKeySequence &k : a->shortcuts()) {
                const QString key = k.toString(QKeySequence::PortableText);
                QVERIFY2(!seen.contains(key), qPrintable(key + ": " + seen.value(key) + " / " + a->text()));
                seen.insert(key, a->text());
            }
        QVERIFY(seen.size() > 30);
    }

    void platformRenameAndOpenDefaults()
    {
        const auto *sc = Shortcuts::instance();
        // Finder's keys on every platform: Return renames, Command/Ctrl+Down opens.
        QCOMPARE(sc->defaults(QStringLiteral("열기")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Down"))});
        QCOMPARE(sc->defaults(QStringLiteral("이름 변경")), QList<QKeySequence>{QKeySequence(QStringLiteral("Return"))});
        QCOMPARE(sc->defaults(QStringLiteral("상위 폴더")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Up"))});
        tab()->selectPaths({p("Alpha")});
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Alpha")});
        key(Qt::Key_Down, Qt::ControlModifier);
        QTRY_COMPARE(tab()->path(), p("Alpha"));
        tab()->navigate(m_tmp.path());
    }

    void listOrderIsFinderLike()
    {
        // Folders first, then files; natural order (file2 before file10). Script order (한글 vs Latin)
        // follows the system locale, like Finder.
        QAbstractItemView *v = tab()->view();
        QStringList names;
        for (int r = 0; r < v->model()->rowCount(v->rootIndex()); ++r)
            names << v->model()->index(r, 0, v->rootIndex()).data().toString();
        QStringList folders = names.mid(0, 3);
        folders.sort();
        QCOMPARE(folders, (QStringList{"Alpha", "Beta", "감마"}));
        QVERIFY(names.indexOf("Alpha") < names.indexOf("Beta"));
        QVERIFY(names.indexOf("file2.go") < names.indexOf("file10.go"));
        QHeaderView *header = qobject_cast<QTreeView *>(tab()->view())->header();
        QCOMPARE(header->logicalIndex(0), int(ColName));
        QCOMPARE(header->logicalIndex(1), int(ColSize));
        QCOMPARE(header->logicalIndex(2), int(ColKind));
        QCOMPARE(header->logicalIndex(3), int(ColDate));
        QCOMPARE(util::humanDate(QDateTime(QDate(2023, 5, 6), QTime(7, 8))), QStringLiteral("2023. 5. 6. 07:08"));
        shot("1-list");
    }

    void keyboardSelectionAndViews()
    {
        tab()->selectPaths({p("Alpha")});
        key(Qt::Key_Down, Qt::ShiftModifier);
        key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(tab()->selectedPaths().size(), 3);

        key(Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(tab()->mode(), BrowserTab::Gallery);
        for (QAction *a : m_win->actions()) // the toolbar's view buttons follow
            if (a->isCheckable() && a->text() == QStringLiteral("갤러리로"))
                QVERIFY(a->isChecked());
            else if (a->isCheckable() && a->text() == QStringLiteral("목록으로"))
                QVERIFY(!a->isChecked());
        QCOMPARE(tab()->selectedPaths().size(), 3); // selection survives the mode switch
        QTest::qWait(800); // thumbnails render in the background
        shot("2-gallery");

        tab()->selectPaths({p("Beta/inner")});
        key(Qt::Key_3, Qt::ControlModifier);
        QCOMPARE(tab()->mode(), BrowserTab::Columns);
        // In column view the selected folder is the current folder (its contents show to the right).
        QTRY_COMPARE(tab()->path(), p("Beta/inner"));
        shot("3-columns");
        key(Qt::Key_Right); // step into it
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Beta/inner/deep.txt")});
        QCOMPARE(tab()->path(), p("Beta/inner"));
        QTest::qWait(600); // column view scrolls with an animation
        shot("3b-columns-file");

        key(Qt::Key_2, Qt::ControlModifier);
        QCOMPARE(tab()->mode(), BrowserTab::List);
        tab()->navigate(m_tmp.path());
        tab()->focusView();
    }

    void quickLookFollowsSelection()
    {
        tab()->selectPaths({p("photo.png")});
        key(Qt::Key_Space);
        auto *ql = m_win->findChild<QuickLookWindow *>();
        QVERIFY(ql);
        QTRY_VERIFY(ql->isVisible());
        QCOMPARE(ql->preview()->path(), p("photo.png"));
        QTest::qWait(400);
        shot("4-quicklook-image", ql);
        QTest::keyClick(ql, Qt::Key_Up); // forwarded to the list
        QTRY_COMPARE(ql->preview()->path(), p("notes.txt"));
        shot("5-quicklook-text", ql);
        QTest::keyClick(ql, Qt::Key_Escape);
        QTRY_VERIFY(!ql->isVisible());
    }

    void renameWithEnterAndUndo()
    {
        tab()->focusView();
        tab()->selectPaths({p("notes.txt")});
        key(Qt::Key_Return);
        QTRY_VERIFY(qobject_cast<QLineEdit *>(focus()));
        auto *le = qobject_cast<QLineEdit *>(focus());
        QTRY_COMPARE(le->selectedText(), QStringLiteral("notes")); // extension not selected
        type("memo");
        QCOMPARE(le->text(), QStringLiteral("memo.txt"));
        key(Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(p("memo.txt")));
        QVERIFY(!QFile::exists(p("notes.txt")));
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("memo.txt")});

        tab()->focusView();
        key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(p("notes.txt")));
        QTRY_VERIFY(App::instance()->canRedo()); // the undo job has finished and been recorded
        key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); // redo
        QTRY_VERIFY(QFile::exists(p("memo.txt")));
    }

    void newFolderStartsRename()
    {
        tab()->focusView();
        key(Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(QDir(p("무제 폴더")).exists());
        QTRY_VERIFY(qobject_cast<QLineEdit *>(focus()));
        type("새폴더");
        key(Qt::Key_Return);
        QTRY_VERIFY(QDir(p("새폴더")).exists());
    }

    void renameSurvivesRowReAdd()
    {
        // Linux's file watcher drops and re-adds a new folder's row right after it appears, which
        // closed the rename editor; it reopens with what was typed. A filter round trip does the same.
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        tab()->navigate(folder.path());
        QTRY_COMPARE(tab()->itemCount(), 0);
        tab()->focusView();
        key(Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(QDir(folder.filePath(QStringLiteral("무제 폴더"))).exists());
        QTRY_VERIFY(qobject_cast<QLineEdit *>(focus()));
        type("다시열기");
        tab()->setSearch(QStringLiteral("zzz-nothing"));
        tab()->setSearch(QString());
        QTRY_VERIFY(qobject_cast<QLineEdit *>(focus()) && qobject_cast<QLineEdit *>(focus())->text() == QStringLiteral("다시열기"));
        key(Qt::Key_Return);
        QTRY_VERIFY(QDir(folder.filePath(QStringLiteral("다시열기"))).exists());
        QDir(folder.filePath(QStringLiteral("다시열기"))).removeRecursively();
        tab()->navigate(m_tmp.path());
    }

    void copyPasteDuplicateTrash()
    {
        tab()->focusView();
        tab()->selectPaths({p("file1.go")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_C, Qt::ControlModifier);
        tab()->selectPaths({p("Alpha")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_Down, Qt::ControlModifier); // ⌘↓ opens the folder
        QTRY_COMPARE(tab()->path(), p("Alpha"));
        key(Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(p("Alpha/file1.go")));
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Alpha/file1.go")});

        key(Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(p("Alpha/file1 복사본.go")));
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Alpha/file1 복사본.go")});

        key(Qt::Key_Backspace, Qt::ControlModifier); // ⌘⌫
        // ⌘Z while the trash job still runs waits for its record (else it would undo the duplicate).
        key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(App::instance()->canRedo());
        QTRY_VERIFY(QFile::exists(p("Alpha/file1 복사본.go"))); // put back from the trash
        QVERIFY(QFile::exists(p("Alpha/file1.go")));

        key(Qt::Key_Up, Qt::ControlModifier); // ⌘↑
        QTRY_COMPARE(tab()->path(), m_tmp.path());
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Alpha")});
        key(Qt::Key_BracketLeft, Qt::ControlModifier); // ⌘[ back
        QTRY_COMPARE(tab()->path(), p("Alpha"));
        key(Qt::Key_BracketRight, Qt::ControlModifier);
        QTRY_COMPARE(tab()->path(), m_tmp.path());
    }

    void searchFilters()
    {
        key(Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit *>(focus()));
        type("file");
        QTRY_COMPARE(tab()->itemCount(), 3);
        shot("6-search");
        key(Qt::Key_Escape);
        QTRY_COMPARE(tab()->itemCount(), 10);
    }

    void newFolderWithSelection()
    {
        tab()->selectPaths({p("file1.go"), p("file10.go")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 2);
        key(Qt::Key_N, kNewFolderWithMods); // ⌃⌘N
        QTRY_VERIFY(QFile::exists(p("항목이 포함된 새로운 폴더/file1.go")));
        QVERIFY(QFile::exists(p("항목이 포함된 새로운 폴더/file10.go")));
        QTRY_VERIFY(qobject_cast<QLineEdit *>(focus())); // straight into rename
        key(Qt::Key_Escape);
        tab()->focusView();
        key(Qt::Key_Z, Qt::ControlModifier); // one undo puts everything back
        QTRY_VERIFY(QFile::exists(p("file1.go")));
        QTRY_VERIFY(!QDir(p("항목이 포함된 새로운 폴더")).exists());
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

    void listSelectionHighlightsBothRowParities()
    {
        const QVariant original = Settings::instance()->value(Settings::ThemeMode);
        const auto restore = qScopeGuard([this, original] {
            Settings::instance()->setValue(Settings::ThemeMode, original);
            Settings::instance()->remove(Settings::FileColorSelection);
            tab()->navigate(m_tmp.path());
            QDir(p("parity")).removeRecursively();
        });
        Settings::instance()->setValue(Settings::ThemeMode, QStringLiteral("dark"));
        Settings::instance()->setValue(Settings::FileColorSelection, false); // the shared bar (item colors: namesBoldAndUppercaseOptions)
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        QVERIFY(tree);
        // Not a QTemporaryDir in the system temp folder: a late listing of that busy folder could drop
        // the brand-new folder's row again, leaving the list without its root (openFolderSurvivesBeingReplaced).
        QVERIFY(makeFixture(QStringLiteral("parity")));
        const QString rows = p("parity");
        QVERIFY(QDir(rows).mkdir(QStringLiteral("A")));
        QVERIFY(QDir(rows).mkdir(QStringLiteral("B")));
        auto *proxy = static_cast<FileProxy *>(tree->model());
        tab()->navigate(rows);
        QTRY_COMPARE(tab()->itemCount(), 2);
        tab()->setMode(BrowserTab::List, true);
        QCOMPARE(tab()->view(), static_cast<QAbstractItemView *>(tree));
        QTRY_COMPARE(proxy->filePath(tree->rootIndex()), rows); // the list shows this folder, not its parents
        Settings::instance()->setValue(Settings::Stripes, true);
        QVERIFY(!tree->alternatingRowColors()); // only Gifiles paints the row backgrounds
        const QPersistentModelIndex first(proxy->indexForPath(QDir(rows).filePath(QStringLiteral("A"))));
        const QPersistentModelIndex second(proxy->indexForPath(QDir(rows).filePath(QStringLiteral("B"))));
        QVERIFY(first.isValid() && second.isValid());
        for (const QPersistentModelIndex &idx : {first, second}) {
            tab()->selectPaths({proxy->filePath(idx)});
            tree->setFocus();
            QTRY_COMPARE(tab()->selectedPaths(), QStringList{proxy->filePath(idx)});
            shot(QStringLiteral("list-selected-row-%1").arg(idx.row()));
            const QImage image = tree->viewport()->grab().toImage();
            const qreal dpr = image.devicePixelRatio();
            for (int column = 0; column < tree->model()->columnCount(); ++column) {
                const QRect cell = tree->visualRect(QModelIndex(idx).siblingAtColumn(column));
                const QPoint sample(cell.center().x(), cell.bottom() - 2);
                const QColor actual = image.pixelColor(qRound(sample.x() * dpr), qRound(sample.y() * dpr));
                QCOMPARE(actual.rgba(), Theme::colors().nameSelection.rgba());
            }
        }
        tree->selectionModel()->select(first, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        shot(QStringLiteral("list-selected-parities"));
        m_win->findChild<QLineEdit *>(QStringLiteral("search"))->setFocus();
        const QImage inactive = tree->viewport()->grab().toImage();
        for (const QPersistentModelIndex &idx : {first, second}) {
            const QRect cell = tree->visualRect(idx);
            const QPoint sample(cell.center().x(), cell.bottom() - 2);
            const qreal dpr = inactive.devicePixelRatio();
            QCOMPARE(inactive.pixelColor(qRound(sample.x() * dpr), qRound(sample.y() * dpr)),
                     Theme::colors().selInactive);
        }
    }

    void dragOnEmptySpaceSelectsRows()
    {
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        QVERIFY(tree);
        auto *proxy = static_cast<FileProxy *>(tree->model());
        const QModelIndex a = proxy->indexForPath(p("Alpha"));
        const QModelIndex b = proxy->indexForPath(p("Beta"));
        QTRY_VERIFY(tree->visualRect(a).isValid() && tree->visualRect(b).isValid());
        // An earlier test may have switched views and folders a moment ago: while the list still runs
        // an expand/collapse animation, Qt ignores the press and anchors the band at the top row.
        QTRY_VERIFY(viewIdle(tree));
        // Start in the date column (not on the name) of Alpha and drag down to Beta.
        const int x = tree->header()->sectionViewportPosition(ColDate) + 20;
        const QPoint from(x, tree->visualRect(a).center().y());
        const QPoint to(x, tree->visualRect(b).center().y());
        QTest::mousePress(tree->viewport(), Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(tree->viewport(), (from + to) / 2);
        QTest::mouseMove(tree->viewport(), to);
        QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, to);
        QTRY_COMPARE(tab()->selectedPaths(), (QStringList{p("Alpha"), p("Beta")}));
    }

    void adjacentSelectedRowsStaySeparate()
    {
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        QVERIFY(tree);
        auto *proxy = static_cast<FileProxy *>(tree->model());
        const QModelIndex a = proxy->indexForPath(p("Alpha"));
        const QModelIndex b = proxy->indexForPath(p("Beta"));
        QTRY_VERIFY(tree->visualRect(a).isValid() && tree->visualRect(b).isValid());
        QCOMPARE(tree->visualRect(b).top(), tree->visualRect(a).bottom() + 1); // neighbors
        tab()->selectPaths({p("Alpha"), p("Beta")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 2);
        const QImage img = tree->viewport()->grab().toImage();
        const int x = tree->header()->sectionViewportPosition(ColDate) + 20;
        const QColor sel = img.pixelColor(x, tree->visualRect(a).center().y());
        QVERIFY(sel != Theme::colors().bg); // painted as selected
        // The last pixel line of the first row is the gap: plain background, not selection.
        QCOMPARE(img.pixelColor(x, tree->visualRect(a).bottom()), Theme::colors().bg);
        QCOMPARE(img.pixelColor(x, tree->visualRect(b).center().y()), sel);
    }

    void listFoldersExpandInPlace()
    {
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        QVERIFY(tree);
        auto *proxy = static_cast<FileProxy *>(tree->model());
        tab()->selectPaths({p("Beta")});
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Beta")});
        const QModelIndex beta = proxy->indexForPath(p("Beta"));
        key(Qt::Key_Right); // expand
        QTRY_VERIFY(tree->isExpanded(beta));
        QCOMPARE(tab()->path(), m_tmp.path()); // still in the same folder
        key(Qt::Key_Right); // step onto the first child
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Beta/inner")});
        key(Qt::Key_Left); // up to the parent row
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Beta")});
        key(Qt::Key_Left); // collapse
        QTRY_VERIFY(!tree->isExpanded(beta));
        // Windows may send the key through the scroll-area viewport; this must still enter,
        // rather than letting the view treat Ctrl+Down as a scroll command.
        QTest::keyClick(tree->viewport(), Qt::Key_Down, Qt::ControlModifier);
        QTRY_COMPARE(tab()->path(), p("Beta"));
    }

    void sidebarFavoritesInsertAtPosition()
    {
        auto *sb = m_win->findChild<Sidebar *>();
        QVERIFY(sb);
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

    void spaceAlwaysClosesQuickLook()
    {
        tab()->selectPaths({p("README.md")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_Space);
        auto *ql = m_win->findChild<QuickLookWindow *>();
        QTRY_VERIFY(ql && ql->isVisible());
        // Give the text view focus: Space must still close, not page down.
        auto *text = ql->findChild<QPlainTextEdit *>();
        QTRY_VERIFY(text && text->isVisible());
        text->setFocus();
        QTest::keyClick(text, Qt::Key_Up); // arrows still move to the neighbouring file
        QTRY_COMPARE(ql->preview()->path(), p("photo.png"));
        QTest::keyClick(ql, Qt::Key_Down);
        QTRY_COMPARE(ql->preview()->path(), p("README.md"));
        QTRY_VERIFY(text->isVisible());
        text->setFocus();
        QTest::keyClick(text, Qt::Key_Space);
        QTRY_VERIFY(!ql->isVisible());
    }

    void copyPasteBothStyles()
    {
        // Windows style: cut, then paste moves. The cut item is dimmed until then.
        tab()->selectPaths({p("file2.go")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_X, Qt::ControlModifier);
        QVERIFY(App::instance()->isCut(p("file2.go")));
        tab()->navigate(p("감마"));
        tab()->focusView();
        key(Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(p("감마/file2.go")));
        QVERIFY(!QFile::exists(p("file2.go")));
        QVERIFY(!App::instance()->isCut(p("file2.go")));

        // Mac style: copy, then decide at paste time: ⌘V copies, ⌥⌘V moves.
        tab()->selectPaths({p("감마/file2.go")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_C, Qt::ControlModifier);
        tab()->navigate(m_tmp.path());
        tab()->focusView();
        key(Qt::Key_V, Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY(QFile::exists(p("file2.go")));
        QVERIFY(!QFile::exists(p("감마/file2.go")));
        tab()->selectPaths({p("file2.go")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_C, Qt::ControlModifier);
        tab()->navigate(p("감마"));
        tab()->focusView();
        key(Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(p("감마/file2.go")));
        QVERIFY(QFile::exists(p("file2.go")));
    }

    void settingsWindow()
    {
        key(Qt::Key_Comma, Qt::ControlModifier); // ⌘,
        SettingsDialog *dlg = nullptr;
        QTRY_VERIFY((dlg = QApplication::activeWindow() ? qobject_cast<SettingsDialog *>(QApplication::activeWindow()) : nullptr)
                    || (dlg = m_win->findChild<SettingsDialog *>()));
        QTRY_VERIFY(dlg->isVisible());
        shot("8-settings", dlg);
        // A switch there takes effect immediately.
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        QVERIFY(tree->property("stripedRows").toBool());
        Settings::instance()->setValue(Settings::Stripes, false);
        QVERIFY(!tree->property("stripedRows").toBool());
        Settings::instance()->setValue(Settings::Stripes, true);
        dlg->close();
    }

    void terminalFollowsAndTakesDroppedPaths()
    {
#if defined(Q_OS_MACOS)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/sh")); // no user rc files
#elif !defined(Q_OS_WIN)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/bash")); // /bin/sh may be dash: no $'...'
#endif
        auto canon = [](const QString &x) { return QFileInfo(x).canonicalFilePath(); };
        TerminalWidget *term = nullptr;
        auto screen = [&term] { return term->screenText().remove(QLatin1Char('\n')); }; // long paths wrap
        key(Qt::Key_QuoteLeft, kTerminalMods); // ⌃` shows the terminal
        QTRY_VERIFY2((term = m_win->findChild<TerminalWidget *>()) && term->isRunning(),
                     qPrintable(term ? term->screenText() : QStringLiteral("no terminal")));
        QTRY_VERIFY2_WITH_TIMEOUT(term->isReady(), qPrintable(term->screenText()), 15000);
        QTRY_COMPARE(canon(term->shellCwd()), canon(m_tmp.path()));

        // Browser → shell: changing folder cd's the shell.
        tab()->navigate(p("Beta"));
        QTRY_VERIFY2(canon(term->shellCwd()) == canon(p("Beta")), qPrintable(term->screenText()));

        // BEL is consumed without disrupting terminal output or subsequent folder following.
        auto *pty = term->findChild<Pty *>();
        QVERIFY(pty);
        pty->dataReceived(QByteArray("\a\r\nsilent-bell-check\a\r\n"));
        QVERIFY(screen().contains(QStringLiteral("silent-bell-check")));
        tab()->navigate(m_tmp.path());
        QTRY_COMPARE(canon(term->shellCwd()), canon(m_tmp.path()));
        tab()->navigate(p("Beta"));
        QTRY_COMPARE(canon(term->shellCwd()), canon(p("Beta")));

        // Selecting items doesn't touch the command line; dragging them onto the terminal types
        // their quoted paths. Enter runs it.
        term->setFocus();
        QTest::keyClicks(term, "echo");
        tab()->selectPaths({p("Beta/inner")});
        tab()->navigate(m_tmp.path()); // waits: the command line isn't empty
        tab()->selectPaths({p("README.md"), p("photo.png")});
        QTest::qWait(300);
        QVERIFY(!screen().contains("echo '"));
        QMimeData drag;
        drag.setUrls({QUrl::fromLocalFile(p("photo.png")), QUrl::fromLocalFile(p("README.md"))});
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction | Qt::MoveAction, &drag, Qt::LeftButton, {});
        QApplication::sendEvent(term, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction | Qt::MoveAction, &drag, Qt::LeftButton, {});
        QApplication::sendEvent(term, &drop);
        QCOMPARE(drop.dropAction(), Qt::CopyAction); // the list never moves the files
        // screenText trims row-end spaces, including the separator at a soft wrap.
        // Check both quoted arguments; the executed echo below checks their values.
        QTRY_VERIFY2(screen().contains("echo") && screen().contains("'" + np("photo.png") + "'")
                     && screen().contains("'" + np("README.md") + "'"),
                     qPrintable(term->screenText()));
        shot("9-terminal");
        term->setFocus();
        const QString outputPath = p("drop-output.txt");
        QString quotedOutput = outputPath;
        quotedOutput.replace(QLatin1String("'"), QLatin1String("''"));
#ifdef Q_OS_WIN
        QTest::keyClicks(term, " | Set-Content -Encoding UTF8 -LiteralPath '" + np("drop-output.txt").replace(QLatin1String("'"), QLatin1String("''")) + "'");
#else
        QTest::keyClicks(term, " > '" + quotedOutput + "'");
#endif
        QTest::keyClick(term, Qt::Key_Return);
        auto output = [&] {
            QFile file(outputPath);
            if (!file.open(QIODevice::ReadOnly))
                return QString();
            return QString::fromUtf8(file.readAll()).remove(QChar(0xFEFF)).trimmed();
        };
#ifdef Q_OS_WIN
        QTRY_COMPARE_WITH_TIMEOUT(output(), np("photo.png") + QStringLiteral("\r\n") + np("README.md"), 15000);
#else
        QTRY_COMPARE(output(), p("photo.png") + QLatin1Char(' ') + p("README.md"));
#endif
        QFile::remove(outputPath);

        // Now idle with an empty line, the deferred cd happens.
        QTRY_COMPARE(canon(term->shellCwd()), canon(m_tmp.path()));

        // Shell → browser: cd in the terminal moves the list.
        QTest::keyClicks(term, "cd Alpha");
        QTest::keyClick(term, Qt::Key_Return);
        QTRY_COMPARE(canon(tab()->path()), canon(p("Alpha")));

        key(Qt::Key_QuoteLeft, kTerminalMods); // folds again; the shell keeps running
        QTRY_VERIFY(!term->isVisible());
        QVERIFY(term->isRunning());
        auto *header = m_win->findChild<QWidget *>(QStringLiteral("termHeader"));
        QVERIFY(header && header->isVisible()); // the bar stays, with an "unfold" chevron
        auto *vsplit = m_win->findChild<QSplitter *>(QStringLiteral("vsplit"));
        auto barAtBottom = [&] {
            return header->parentWidget()->height() <= header->height() + 1
                && header->mapTo(vsplit, QPoint(0, header->height())).y() >= vsplit->height() - 1;
        };
        QTRY_VERIFY(barAtBottom()); // folded down to the bottom edge, the list gets the space
        QTest::mouseClick(header, Qt::LeftButton, {}, QPoint(header->width() / 2, header->height() / 2));
        QTRY_VERIFY(term->isVisible() && header->parentWidget()->height() > 80);
        QTest::mouseClick(header, Qt::LeftButton, {}, QPoint(header->width() / 2, header->height() / 2));
        QTRY_VERIFY(!term->isVisible());
        QTRY_VERIFY(barAtBottom());

        // Dragging the folded bar's edge up unfolds the terminal at the dragged height.
        QSplitterHandle *edge = vsplit->handle(1);
        const QPoint grip(edge->width() / 2, edge->height() / 2);
        QTest::mousePress(edge, Qt::LeftButton, {}, grip);
        QTest::mouseMove(edge, grip - QPoint(0, 200));
        QTest::mouseRelease(edge, Qt::LeftButton, {}, grip - QPoint(0, 200));
        QTRY_VERIFY(term->isVisible());
        QVERIFY2(qAbs(vsplit->sizes().value(1) - (header->height() + 200)) <= 6, qPrintable(QString::number(vsplit->sizes().value(1))));
        // ... and dragging it back down to the bar folds it.
        const QPoint grip2(edge->width() / 2, edge->height() / 2);
        QTest::mousePress(edge, Qt::LeftButton, {}, grip2);
        QTest::mouseMove(edge, grip2 + QPoint(0, 400));
        QTest::mouseRelease(edge, Qt::LeftButton, {}, grip2 + QPoint(0, 400));
        QTRY_VERIFY(!term->isVisible());
        QTRY_VERIFY(barAtBottom());
        Settings::instance()->setValue(Settings::TermShell, QString());
    }

    void revealInFileManager()
    {
        // Folders open as themselves; files open their folder with the file selected.
        const util::Command dir = util::revealCommand(p("Alpha"));
        const util::Command file = util::revealCommand(p("README.md"));
#if defined(Q_OS_MACOS)
        QCOMPARE(dir.args, QStringList{p("Alpha")});
        QCOMPARE(file.args, (QStringList{QStringLiteral("-R"), p("README.md")}));
#elif defined(Q_OS_WIN)
        QCOMPARE(dir.args, QStringList{np("Alpha")});
        QCOMPARE(file.args, QStringList{QStringLiteral("/select,") + np("README.md")});
#else
        QVERIFY(dir.args.contains(QStringLiteral("org.freedesktop.FileManager1.ShowFolders")));
        QVERIFY(file.args.contains(QStringLiteral("org.freedesktop.FileManager1.ShowItems")));
        QVERIFY(file.args.contains(QStringLiteral("array:string:") + QUrl::fromLocalFile(p("README.md")).toString(QUrl::FullyEncoded)));
#endif
        bool found = false;
        for (QAction *a : m_win->actions())
            found = found || a->text() == util::revealActionText();
        QVERIFY(found);
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

    void terminalTabsTakeOverWhileBusy()
    {
#if defined(Q_OS_MACOS)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/sh"));
#elif !defined(Q_OS_WIN)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/bash"));
#endif
        constexpr Qt::KeyboardModifiers kTabMods = Qt::ControlModifier; // ⌘ on macOS, Ctrl elsewhere
        auto canon = [](const QString &x) { return QFileInfo(x).canonicalFilePath(); };
        auto current = [this] {
            auto *stack = m_win->findChild<QStackedWidget *>(QStringLiteral("termStack"));
            return stack ? qobject_cast<TerminalWidget *>(stack->currentWidget()) : nullptr;
        };
        auto *bar = m_win->findChild<QTabBar *>(QStringLiteral("termTabs"));
        QVERIFY(bar);
        tab()->navigate(m_tmp.path());
        key(Qt::Key_QuoteLeft, kTerminalMods);
        QPointer<TerminalWidget> first = current();
        QTRY_VERIFY(first && first->isVisible() && first->isRunning());
        QTRY_VERIFY2_WITH_TIMEOUT(first->isReady(), qPrintable(first->screenText()), 15000);
        QTRY_COMPARE(canon(first->shellCwd()), canon(m_tmp.path()));
        const int before = bar->count();

        // A program runs in the current tab: the browser's next folder opens in a new tab.
        first->setFocus();
#ifdef Q_OS_WIN
        QTest::keyClicks(first, "ping -n 30 127.0.0.1 > $null");
#else
        QTest::keyClicks(first, "sleep 30");
#endif
        QTest::keyClick(first, Qt::Key_Return);
        QTRY_VERIFY2(first->isBusy(), qPrintable(first->screenText()));
        tab()->navigate(p("Beta"));
        QTRY_COMPARE(bar->count(), before + 1);
        QPointer<TerminalWidget> second = current();
        QVERIFY(second && second != first);
        QTRY_VERIFY(second->isRunning());
        QTRY_VERIFY2_WITH_TIMEOUT(second->isReady(), qPrintable(second->screenText()), 15000);
        QTRY_COMPARE(canon(second->shellCwd()), canon(p("Beta")));
        QCOMPARE(canon(first->shellCwd()), canon(m_tmp.path())); // the busy one stays put
        QCOMPARE(bar->tabText(bar->currentIndex()), second->tabTitle());

        // The new (idle) tab follows the browser from now on.
        tab()->navigate(p("Alpha"));
        QTRY_COMPARE(canon(second->shellCwd()), canon(p("Alpha")));
        QCOMPARE(bar->count(), before + 1);

        // Switching tabs from the keyboard shows that shell's folder in the list.
        second->setFocus();
        key(Qt::Key_Tab, kTerminalMods); // ⌃Tab
        QTRY_COMPARE(current(), first.data());
        QTRY_COMPARE(canon(tab()->path()), canon(m_tmp.path()));
        QTRY_VERIFY(first->hasFocus());

        // ⌘W in the terminal closes the terminal tab (ending its program), not the browser tab.
        const int browserTabs = m_win->findChild<QTabWidget *>()->count();
        key(Qt::Key_W, kTabMods);
        QTRY_VERIFY(!first);
        QCOMPARE(bar->count(), before);
        QCOMPARE(m_win->findChild<QTabWidget *>()->count(), browserTabs);
        QTRY_COMPARE(current(), second.data());

        // ⌘T in the terminal opens another terminal tab at the browser's folder.
        second->setFocus();
        key(Qt::Key_T, kTabMods);
        QTRY_COMPARE(bar->count(), before + 1);
        QPointer<TerminalWidget> third = current();
        QVERIFY(third != second);
        QTRY_VERIFY(third->hasFocus());
        QCOMPARE(m_win->findChild<QTabWidget *>()->count(), browserTabs);
        emit bar->tabCloseRequested(bar->currentIndex()); // the tab's close button
        QTRY_VERIFY(!third);
        QCOMPARE(bar->count(), before);

        key(Qt::Key_QuoteLeft, kTerminalMods); // fold again
        QTRY_VERIFY(!second->isVisible());
        Settings::instance()->setValue(Settings::TermShell, QString());
    }

    void aiRequestRunsInTerminal()
    {
        // "AI로 실행" asks for the request first; here a stand-in for claude -p that just
        // prints what it receives.
        QVariantList commands = Settings::instance()->value(Settings::SelectionCommands).toList();
        for (QVariant &c : commands) {
            QVariantMap m = c.toMap();
            if (m.value(QStringLiteral("id")).toString() != QLatin1String("ai"))
                continue;
#ifdef Q_OS_WIN
            m.insert(QStringLiteral("command"), QStringLiteral("Write-Output ('AI<' + {prompt} + '>'); Write-Output ('AI<' + {files} + '>')"));
#else
            m.insert(QStringLiteral("command"), QStringLiteral("printf 'AI<%s>\\n' {prompt} {files}"));
#endif
            c = m;
        }
        Settings::instance()->setValue(Settings::SelectionCommands, commands);
        tab()->selectPaths({p("photo.png")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        m_win->runSelectionCommand(Settings::instance()->selectionCommand(QStringLiteral("ai")), tab()->selectedPaths());
        QPointer<QLineEdit> prompt;
        QTRY_VERIFY((prompt = m_win->findChild<QLineEdit *>(QStringLiteral("aiPrompt"))) && prompt->isVisible());
        QTRY_VERIFY(prompt->hasFocus());
        QVERIFY(prompt->placeholderText().contains(QStringLiteral("1개")));
        QTest::keyClicks(prompt, "it's \"fine\"");
        QTest::keyClick(prompt, Qt::Key_Return);
        TerminalWidget *term = m_win->findChild<TerminalWidget *>();
        QTRY_VERIFY(term && term->isVisible()); // the terminal unfolds to show the run
        auto screen = [&term] { return term->screenText().remove(QLatin1Char('\n')); };
        QTRY_VERIFY2(screen().contains(QStringLiteral("AI<it's \"fine\"")), qPrintable(term->screenText())); // quotes survive
        QTRY_VERIFY(screen().contains(np("photo.png") + QStringLiteral(">")));  // the selected path follows
        Settings::instance()->remove(Settings::SelectionCommands);
        QTRY_VERIFY(!prompt); // the popup closed (and went away) on Enter
        QTRY_VERIFY2(term->hasFocus(), qPrintable(QStringLiteral("active=%1 focus=%2").arg(
            QApplication::activeWindow() ? QApplication::activeWindow()->metaObject()->className() : "none",
            QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "none"))); // the terminal took the keyboard
        key(Qt::Key_QuoteLeft, kTerminalMods); // fold again
        QTRY_VERIFY(!term->isVisible());
    }

    void folderRemembersItsView()
    {
        // Gallery chosen in Alpha sticks to Alpha; the parent keeps its own (list) view.
        tab()->navigate(m_tmp.path());
        key(Qt::Key_2, Qt::ControlModifier);
        tab()->navigate(p("Alpha"));
        tab()->focusView();
        key(Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(tab()->mode(), BrowserTab::Gallery);
        tab()->navigate(m_tmp.path());
        QCOMPARE(tab()->mode(), BrowserTab::List);
        tab()->navigate(p("Alpha"));
        QCOMPARE(tab()->mode(), BrowserTab::Gallery);

        // Sorting is remembered too.
        tab()->navigate(m_tmp.path());
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        tree->sortByColumn(ColSize, Qt::DescendingOrder);
        QTest::qWait(500); // saved after a short debounce
        tab()->navigate(p("Beta"));
        tree->sortByColumn(ColName, Qt::AscendingOrder);
        tab()->navigate(m_tmp.path()); // at once: Beta's pending save must not land on this folder
        QTest::qWait(500);
        QCOMPARE(tree->header()->sortIndicatorSection(), int(ColSize));
        QCOMPARE(tree->header()->sortIndicatorOrder(), Qt::DescendingOrder);
        tree->sortByColumn(ColName, Qt::AscendingOrder);
        QTest::qWait(500);
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

    void quickLookShowsNaturalSizeAndResizes()
    {
        QDir().mkpath(p(QStringLiteral("zoom")));
        QImage small(120, 80, QImage::Format_RGB32);
        small.fill(Qt::darkCyan);
        QVERIFY(small.save(p(QStringLiteral("zoom/small.png"))));
        const QRect avail = m_win->screen()->availableGeometry();
        QImage big(avail.width() * 2, 300, QImage::Format_RGB32);
        big.fill(Qt::darkMagenta);
        QVERIFY(big.save(p(QStringLiteral("zoom/wide.png"))));
        tab()->navigate(p(QStringLiteral("zoom")));
        tab()->setMode(BrowserTab::List, true);
        QTRY_COMPARE(tab()->itemCount(), 2);
        tab()->focusView();

        // A small image is shown at its own size (the remembered Quick Look scale, 100%).
        Settings::instance()->setValue(Settings::QuickLookScale, 100);
        tab()->selectPaths({p(QStringLiteral("zoom/small.png"))});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_Space);
        auto *ql = m_win->findChild<QuickLookWindow *>();
        // The size comes from the file header: the window appears at its final size right away
        // and keeps it when the decoded image arrives.
        QVERIFY(ql && ql->isVisible());
        ZoomArea *area = ql->findChild<ZoomArea *>();
        QVERIFY(area && area->naturalSize() == QSize(120, 80));
        const QSize shownAt = ql->size();
        QTRY_COMPARE(area->widget()->size(), QSize(120, 80));
        auto hasResolution = [&] {
            for (QLabel *l : ql->findChildren<QLabel *>())
                if (l->isVisible() && l->text().contains(QStringLiteral("120 × 80")))
                    return true;
            return false;
        };
        QTRY_VERIFY(hasResolution()); // shown in the details line
        QTRY_VERIFY(!qobject_cast<QLabel *>(area->widget())->pixmap().isNull()); // decoded
        QCOMPARE(ql->size(), shownAt);
        QVERIFY(!area->isZoomable()); // already 100%: no magnifier
        // + / − resize it, and the size is remembered for the next image.
        QTest::keyClick(ql, Qt::Key_Equal); // +
        QTRY_COMPARE(area->widget()->size(), QSize(150, 100));
        QCOMPARE(Settings::instance()->value(Settings::QuickLookScale).toInt(), 125);
        QTest::keyClick(ql, Qt::Key_Equal, Qt::ControlModifier); // ⌘+ too
        QTRY_COMPARE(area->widget()->size(), QSize(186, 124)); // 155% (steps round to 5%)
        QTest::keyClick(ql, Qt::Key_Minus, Qt::ControlModifier); // ⌘−
        QTest::keyClick(ql, Qt::Key_Minus);
        QTRY_COMPARE(area->widget()->size(), QSize(120, 80));
        QCOMPARE(Settings::instance()->value(Settings::QuickLookScale).toInt(), 100);

        // A wider-than-screen image is shrunk to 90% of the screen width; a click shows 100%.
        // (an earlier test may have left the sort descending)
        QTest::keyClick(ql, tab()->view()->currentIndex().row() == 0 ? Qt::Key_Down : Qt::Key_Up);
        QTRY_COMPARE(ql->preview()->path(), p(QStringLiteral("zoom/wide.png")));
        QTRY_COMPARE(area->naturalSize(), big.size());
        QTRY_VERIFY(area->widget()->width() <= avail.width() * 9 / 10 + 1);
        QVERIFY(ql->width() <= avail.width());
        QVERIFY(area->isZoomable());
        QTest::mouseClick(area->widget(), Qt::LeftButton, {}, area->widget()->rect().center());
        QVERIFY(area->isActualSize());
        QCOMPARE(area->widget()->size(), big.size());
        QTest::mouseClick(area->viewport(), Qt::LeftButton, {}, area->viewport()->rect().center());
        QVERIFY(!area->isActualSize());

        // A 144 dpi PNG is shown at half its pixel size, known from its header before decoding.
        QTest::keyClick(ql, Qt::Key_Escape);
        QTRY_VERIFY(!ql->isVisible());
        QImage retina(400, 200, QImage::Format_RGB32); // a 144 dpi screenshot
        retina.fill(Qt::darkGreen);
        retina.setDotsPerMeterX(5669);
        retina.setDotsPerMeterY(5669);
        QVERIFY(retina.save(p(QStringLiteral("zoom/shot.png"))));
        QTRY_COMPARE(tab()->itemCount(), 3);
        tab()->focusView();
        tab()->selectPaths({p(QStringLiteral("zoom/shot.png"))});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_Space);
        QVERIFY(ql->isVisible());
        QCOMPARE(area->naturalSize(), QSize(200, 100));
        QTest::qWait(300);
        QCOMPARE(area->naturalSize(), QSize(200, 100));
        QTest::keyClick(ql, Qt::Key_Escape);
        QTRY_VERIFY(!ql->isVisible());
        QVERIFY(!area->viewport()->testAttribute(Qt::WA_SetCursor)); // no magnifier left behind
        tab()->navigate(m_tmp.path());
        QDir(p(QStringLiteral("zoom"))).removeRecursively();
    }

    void archivesExpandInPlace()
    {
        QDir().mkpath(p(QStringLiteral("arch/src/pack")));
        write(QStringLiteral("arch/src/pack/a.txt"), "a\n");
        write(QStringLiteral("arch/src/b.txt"), "b\n");
        auto zip = [&](const QString &out, const QStringList &items) {
            QProcess z;
            z.setWorkingDirectory(p(QStringLiteral("arch/src")));
#ifdef Q_OS_WIN
            z.start(QStringLiteral("tar.exe"), QStringList{QStringLiteral("-a"), QStringLiteral("-cf"), out} + items); // bsdtar writes zip by suffix
#else
            z.start(QStringLiteral("zip"), QStringList{QStringLiteral("-qr"), out} + items);
#endif
            QVERIFY(z.waitForFinished() && z.exitCode() == 0);
        };
        zip(p(QStringLiteral("arch/one.zip")), {QStringLiteral("pack")});
        zip(p(QStringLiteral("arch/many.zip")), {QStringLiteral("pack"), QStringLiteral("b.txt")});
        QDir().mkpath(p(QStringLiteral("arch/pack"))); // name taken: the result gets "pack 2"
        tab()->navigate(p(QStringLiteral("arch")));
        QTRY_COMPARE(tab()->itemCount(), 4);
        tab()->focusView();

        tab()->selectPaths({p(QStringLiteral("arch/one.zip"))});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        QCOMPARE(openKey(true), QStringLiteral("열기 (O)")); // open, from the menu ⌘↓ shows on files
        QTRY_VERIFY(QFile::exists(p(QStringLiteral("arch/pack 2/a.txt"))));
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p(QStringLiteral("arch/pack 2"))});

        tab()->selectPaths({p(QStringLiteral("arch/many.zip"))});
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p(QStringLiteral("arch/many.zip"))});
        openKey(true);
        QTRY_VERIFY(QFile::exists(p(QStringLiteral("arch/many/b.txt"))));
        QVERIFY(QFile::exists(p(QStringLiteral("arch/many/pack/a.txt"))));
        // The job's undo record lands with the selection, after the files appear on disk.
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p(QStringLiteral("arch/many"))});
        QVERIFY(QDir(p(QStringLiteral("arch"))).entryList({QStringLiteral(".gifiles-extract-*")}, QDir::AllEntries | QDir::Hidden).isEmpty());

        // Undo moves the extracted folder to the trash.
        QTRY_VERIFY(App::instance()->canUndo());
        key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(!QFile::exists(p(QStringLiteral("arch/many"))));
        tab()->navigate(m_tmp.path());
        QDir(p(QStringLiteral("arch"))).removeRecursively();
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

    void namesBoldAndUppercaseOptions()
    {
        // 모양 → 파일 이름: 굵게 보기, 항상 대문자로 표시 (both off by default; shown only, real names stay).
        Settings *st = Settings::instance();
        QVERIFY(!st->flag(Settings::BoldNames));
        QVERIFY(!st->flag(Settings::UppercaseNames));
        QTemporaryDir dir; // before the guard: the browser leaves it before it is removed
        QVERIFY(dir.isValid());
        const auto restore = qScopeGuard([this, st] {
            st->remove(Settings::BoldNames);
            st->remove(Settings::UppercaseNames);
            tab()->navigate(m_tmp.path());
        });
        QFile f(dir.filePath(QStringLiteral("readme.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        tab()->setMode(BrowserTab::List, true);
        auto *proxy = static_cast<FileProxy *>(tab()->view()->model());
        tab()->navigate(dir.path());
        QTRY_VERIFY(proxy->indexForPath(f.fileName()).isValid());
        const QModelIndex idx = proxy->indexForPath(f.fileName());
        struct Peek : ItemDelegate {
            using ItemDelegate::ItemDelegate;
            QString text(const QModelIndex &i) const
            {
                QStyleOptionViewItem o;
                initStyleOption(&o, i);
                return o.text;
            }
        } peek(proxy, ItemDelegate::List, nullptr);
        auto editorBold = [&] {
            std::unique_ptr<QWidget> e(peek.createEditor(tab()->view()->viewport(), QStyleOptionViewItem(), idx));
            return e->font().bold();
        };
        QCOMPARE(peek.text(idx), QStringLiteral("readme.txt"));
        QVERIFY(!editorBold());
        st->setValue(Settings::UppercaseNames, true);
        st->setValue(Settings::BoldNames, true);
        QCOMPARE(peek.text(idx), QStringLiteral("README.TXT"));
        QCOMPARE(idx.data().toString(), QStringLiteral("readme.txt")); // the name itself is untouched
        QVERIFY(editorBold()); // the editor matches the bold row, so nothing moves when renaming starts
        // By default selected names keep their colors on one accent bar; with file_colors.selection the
        // bar is the item's own color (as given, either theme) dimmed to 80% of its lightness.
        QCOMPARE(peek.selectionColor(idx), Theme::colors().nameSelection);
        st->setValue(Settings::FileColorSelection, true);
        QCOMPARE(peek.selectionColor(idx), Theme::selectionFill(Theme::fileBaseColor(QStringLiteral("readme.txt"))));
        QVERIFY(peek.selectionColor(idx).isValid());
        QFile plain(dir.filePath(QStringLiteral("Makefile")));
        QVERIFY(plain.open(QIODevice::WriteOnly));
        plain.close();
        QTRY_VERIFY(proxy->indexForPath(plain.fileName()).isValid());
        QCOMPARE(peek.selectionColor(proxy->indexForPath(plain.fileName())), Theme::selectionFill(Theme::plainSelectionColor())); // no color: gray
        st->setValue(Settings::FileColorSelection, false);
        QCOMPARE(peek.selectionColor(idx), Theme::colors().nameSelection);
        st->remove(Settings::FileColorSelection);
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
        QTRY_VERIFY(!dlg); // gone, and the main window active again (a late activation closes popups)
        QTest::qWait(100);
        // Saved in config.toml, active in the window: the new key opens, the old one no longer does.
        auto saved = [] {
            QFile cfg(Settings::configPath());
            return cfg.open(QIODevice::ReadOnly) && QString::fromUtf8(cfg.readAll()).contains(QStringLiteral("\"열기\" = [\"Ctrl+J\"]"));
        };
        QTRY_VERIFY(saved()); // a busy file on Windows is saved on a retry
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
        tab()->focusView();
        tab()->selectPaths({p("Alpha")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_J, Qt::ControlModifier);
        QTRY_COMPARE(tab()->path(), p("Alpha"));
        tab()->navigate(m_tmp.path());
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

    void openKeyOnSeveralItemsShowsMenu()
    {
        tab()->focusView();
        tab()->selectPaths({p("Alpha"), p("Beta")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 2);
        QCOMPARE(openKey(false), QStringLiteral("열기 (2개 항목) (O)"));
        QCOMPARE(tab()->path(), m_tmp.path()); // nothing opened
        write(QStringLiteral("single.txt"), "x\n");
        tab()->selectPaths({p("single.txt")}); // a single file: the menu too
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        QCOMPARE(openKey(false), QStringLiteral("열기 (O)"));
        tab()->selectPaths({p("Alpha")}); // a single folder is entered at once
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        QCOMPARE(openKey(false), QString());
        QTRY_COMPARE(tab()->path(), p("Alpha"));
        tab()->navigate(m_tmp.path());
        QFile::remove(p("single.txt"));
    }

    void configFileEditsApply()
    {
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        QVERIFY(tree && tree->property("stripedRows").toBool());
        const QString path = Settings::configPath();
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString original = QString::fromUtf8(f.readAll());
        f.close();
        QVERIFY(original.contains(QStringLiteral("stripes = true")));
        auto writeConfig = [&](const QString &text) {
            QVERIFY(replaceFile(path, text.toUtf8())); // like an editor: replace the file
        };
        writeConfig(QString(original).replace(QStringLiteral("stripes = true"), QStringLiteral("stripes = false")));
        QTRY_VERIFY(!tree->property("stripedRows").toBool()); // applied without restarting
        // A wrong value is reported and the old one kept.
        writeConfig(QString(original).replace(QStringLiteral("stripes = true"), QStringLiteral("stripes = \"yes\"")));
        QTRY_VERIFY(!Settings::instance()->problems().isEmpty());
        QVERIFY(Settings::instance()->problems().first().contains(QStringLiteral("view.stripes")));
        QVERIFY(!tree->property("stripedRows").toBool());
        QVERIFY(!Settings::check(QStringLiteral("[view]\nstripes = \"yes\"\n")).isEmpty());
        QVERIFY(!Settings::check(QStringLiteral("[view\n")).isEmpty());
        QVERIFY(Settings::check(original).isEmpty());
        for (QWidget *w : QApplication::topLevelWidgets()) // the notice about it
            if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible())
                box->close();
        writeConfig(original);
        QTRY_VERIFY(tree->property("stripedRows").toBool());
        QTRY_VERIFY(Settings::instance()->problems().isEmpty());
    }

    void selectionCommandsQuoteEveryName()
    {
        // Spaces, quotes, $, `, Korean, a newline: each path stays one shell word, on one line.
        const QString dir = QDir::cleanPath(m_tmp.path());
        const QStringList paths = {dir + QStringLiteral("/a b.txt"), dir + QStringLiteral("/한글 '따옴표'.txt"),
                                   dir + QStringLiteral("/x$y`z\n.txt")};
        const QString out = TerminalWidget::expandCommand(QStringLiteral("cmd {names} | {dir} | {prompt} | {nope}"), paths,
                                                          QStringLiteral("요청 \"{files}\""));
        QVERIFY(!out.contains(QLatin1Char('\n')));
#ifdef Q_OS_WIN
        QCOMPARE(out, QStringLiteral("cmd \"a b.txt\", \"한글 'Tick'.txt\", \"x`$y``z`n.txt\" | \"%1\" | \"요청 `\"{files}`\"\" | {nope}")
                          .arg(QDir::toNativeSeparators(dir)).replace(QStringLiteral("Tick"), QStringLiteral("따옴표")));
#else
        QCOMPARE(out, QStringLiteral("cmd $'a b.txt' $'한글 \\'따옴표\\'.txt' $'x$y`z\\n.txt' | $'%1' | $'요청 \"{files}\"' | {nope}").arg(dir));
#endif
    }

    void selectionCommandRunsInTerminal()
    {
#if defined(Q_OS_MACOS)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/sh")); // no user rc files
#elif !defined(Q_OS_WIN)
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/bin/bash"));
#endif
        QDir().mkpath(p("cmdtest"));
        const QStringList names = {QStringLiteral("a b.txt"), QStringLiteral("한글 '따옴표'.txt"), QStringLiteral("it's $HOME `x`.txt")};
        QStringList paths;
        for (const QString &n : names) {
            write(QStringLiteral("cmdtest/") + n, "x\n");
            paths << p(QStringLiteral("cmdtest/") + n);
        }
        // The built-in "새로운 폴더" command, as config.toml lists it on this platform: it runs
        // quietly (terminal = false), without opening the terminal.
        const QVariantMap folder = Settings::instance()->selectionCommand(QStringLiteral("new_folder"));
        QCOMPARE(folder.value(QStringLiteral("terminal")).toBool(), false);
        const bool termOpen = m_win->findChild<TerminalWidget *>() && m_win->findChild<TerminalWidget *>()->isVisible();
        tab()->navigate(p("cmdtest"));
        m_win->runSelectionCommand(folder, paths);
        for (const QString &n : names)
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(p(QStringLiteral("cmdtest/새 폴더/") + n)), 20000);
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("cmdtest/새 폴더")}); // the new folder is selected
        QCOMPARE(m_win->findChild<TerminalWidget *>() && m_win->findChild<TerminalWidget *>()->isVisible(), termOpen);
        // The same command with terminal = true runs in the terminal.
        QStringList moved;
        for (const QString &n : names)
            moved << p(QStringLiteral("cmdtest/새 폴더/") + n);
        QVariantMap shown = folder;
        shown.insert(QStringLiteral("terminal"), true);
        m_win->runSelectionCommand(shown, moved);
        for (const QString &n : names)
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(p(QStringLiteral("cmdtest/새 폴더/새 폴더/") + n)), 20000);
        TerminalWidget *term = m_win->findChild<TerminalWidget *>();
        QVERIFY(term && term->isVisible());
        key(Qt::Key_QuoteLeft, kTerminalMods); // fold again
        QTRY_VERIFY(!term->isVisible());
        tab()->navigate(m_tmp.path());
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

    void openInNewTabOnlyForFolders()
    {
        // Files have no tab to open in: the item is offered only when a folder is selected.
        tab()->navigate(m_tmp.path());
        write(QStringLiteral("tabonly.txt"), "x\n");
        auto offered = [&] {
            bool found = false;
            openKey(false, [&](QMenu *menu) {
                for (QAction *a : menu->actions())
                    found = found || a->objectName() == QStringLiteral("새로운 탭에서 열기");
            }, true);
            return found;
        };
        tab()->selectPaths({p("tabonly.txt")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        QVERIFY(!tab()->hasSelectedFolder());
        QVERIFY(!offered());
        tab()->selectPaths({p("tabonly.txt"), p("Alpha")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 2);
        QVERIFY(tab()->hasSelectedFolder());
        QVERIFY(offered());
        QFile::remove(p("tabonly.txt"));
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

    void contextMenuLetters()
    {
        tab()->navigate(m_tmp.path());
        write(QStringLiteral("letters.txt"), "x\n");
        tab()->selectPaths({p("letters.txt")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        // ㄷ (the E key on the Korean layout) = E: 새로운 탭에서 열기, here two folders.
        tab()->selectPaths({p("Alpha"), p("Beta")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 2);
        auto *tabs = m_win->findChild<QTabWidget *>();
        const int before = tabs->count();
        QString seen; // what the menu offered, for a failure message
        const QString first = openKey(false, [&](QMenu *menu) {
            for (QAction *a : menu->actions())
                if (a->property("menuLetter").toChar() == QLatin1Char('E'))
                    seen += a->text() + (a->isEnabled() ? QStringLiteral(" on") : QStringLiteral(" off"));
            seen += QStringLiteral(" | sel %1").arg(tab()->selectedPaths().size());
            QKeyEvent k(QEvent::KeyPress, 0, Qt::NoModifier, QStringLiteral("ㄷ"));
            seen += QCoreApplication::sendEvent(menu, &k) ? QStringLiteral(" | taken") : QStringLiteral(" | ignored");
            seen += QStringLiteral(" | tabs %1").arg(tabs->count());
        }, true);
        QTRY_VERIFY2(tabs->count() == before + 2, qPrintable(first + QStringLiteral(" | ") + seen));
        while (tabs->count() > before)
            tabs->tabCloseRequested(tabs->count() - 1);
        tabs->setCurrentIndex(0);
        tab()->selectPaths({p("letters.txt")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        // S opens "선택한 항목들로…" with its first command chosen; the names are back afterwards.
        QString sub, active;
        openKey(false, [&](QMenu *menu) {
            QTest::keyClick(menu, Qt::Key_S);
            if (auto *m = qobject_cast<QMenu *>(QApplication::activePopupWidget()); m && m != menu) {
                sub = m->title();
                active = m->activeAction() ? m->activeAction()->text() : QString();
                m->close();
            }
        }, true);
        QCOMPARE(sub, QStringLiteral("선택한 항목들로… (S)"));
        QCOMPARE(active, QStringLiteral("새로운 폴더 (N)"));
        for (QAction *a : m_win->actions())
            if (a->objectName() == QStringLiteral("경로 복사"))
                QCOMPARE(a->text(), QStringLiteral("경로 복사")); // the letter was only for the menu
        const QString id = QStringLiteral("컨텍스트 메뉴 새로운 탭에서 열기");
        const auto restore = qScopeGuard([id] { Settings::instance()->remove(QStringLiteral("shortcuts/") + id); });
        Shortcuts::instance()->assign(id, QKeySequence(QStringLiteral("F8")));
        tab()->selectPaths({p("Alpha"), p("Beta")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 2);
        openKey(false, [&](QMenu *menu) {
            QTest::keyClick(menu, Qt::Key_E); // the old menu key no longer runs the command
            QVERIFY(menu->isVisible());
            QCOMPARE(tabs->count(), before);
            QTest::keyClick(menu, Qt::Key_F8);
        }, true);
        QTRY_COMPARE(tabs->count(), before + 2);
        while (tabs->count() > before)
            tabs->tabCloseRequested(tabs->count() - 1);
        tabs->setCurrentIndex(0);
        QFile::remove(p("letters.txt"));
        const QString gallery = QStringLiteral("컨텍스트 메뉴 갤러리");
        const auto restoreGallery = qScopeGuard([gallery] { Settings::instance()->remove(QStringLiteral("shortcuts/") + gallery); });
        Shortcuts::instance()->assign(gallery, QKeySequence(QStringLiteral("F9")));
        tab()->setMode(BrowserTab::List, true);
        QTimer::singleShot(100, this, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(menu);
            QTest::keyClick(menu, Qt::Key_1);
            QCOMPARE(tab()->mode(), BrowserTab::List);
            QTest::keyClick(menu, Qt::Key_F9);
            menu->close();
        });
        emit tab()->contextMenuRequested(m_win->mapToGlobal(QPoint(300, 200)), false);
        QCOMPARE(tab()->mode(), BrowserTab::Gallery);
        tab()->setMode(BrowserTab::List, true);
    }

    void closeKeyClosesTheFrontWindowOnly()
    {
        // macOS's menu bar sends ⌘W to the main window's action while Settings is in front.
        const int tabs = m_win->findChild<QTabWidget *>()->count();
        SettingsDialog::showSingleton(m_win);
        QPointer<QWidget> dlg;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<SettingsDialog *>(w) && w->isVisible())
                dlg = w;
        QVERIFY(dlg);
        QTest::qWait(100); // a late activation of the main window may still be on its way
        dlg->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(dlg));
        QTest::qWait(50);
        QCOMPARE(QApplication::activeWindow(), dlg.data()); // else the action would close the tab
        bool found = false;
        for (QAction *a : m_win->actions())
            if (a->objectName() == QStringLiteral("탭 닫기")) {
                found = true;
                a->trigger();
            }
        QVERIFY(found);
        QTRY_VERIFY2(!dlg, QApplication::activeWindow() ? QApplication::activeWindow()->metaObject()->className() : "none");
        QVERIFY(m_win->isVisible());
        QCOMPARE(m_win->findChild<QTabWidget *>()->count(), tabs);
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
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

    void quickLookReopensMedia()
    {
        // Closing stops the player and drops its source; opening the same file again must load it again.
        write(QStringLiteral("again.mp3"), QByteArray(256, '\0'));
        tab()->selectPaths({p("again.mp3")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        tab()->focusView();
        for (int i = 0; i < 3; ++i) {
            key(Qt::Key_Space);
            auto *ql = m_win->findChild<QuickLookWindow *>();
            QTRY_VERIFY(ql && ql->isVisible());
            auto *player = ql->findChild<QMediaPlayer *>();
            QVERIFY(player);
            QCOMPARE(player->source(), QUrl::fromLocalFile(p("again.mp3")));
            QTest::keyClick(ql, Qt::Key_Space);
            QTRY_VERIFY(!ql->isVisible());
            QVERIFY(player->source().isEmpty());
            tab()->focusView();
        }
        QFile::remove(p("again.mp3"));
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
                firstShown = f.startTime() / 1000;
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

    void quickLookRemembersDraggedSize()
    {
        // Dragging the window's edge is remembered per kind: an image as its scale against the
        // original, a document and a sound as the window size (the document's text stays).
        Settings *s = Settings::instance();
        s->setValue(Settings::QuickLookScale, 100);
        s->setValue(Settings::QuickLookDocScale, 100);
        const auto restore = qScopeGuard([s] {
            s->setValue(Settings::QuickLookScale, 100);
            for (const char *k : {Settings::QuickLookDocWidth, Settings::QuickLookDocHeight, Settings::QuickLookAudioWidth,
                                  Settings::QuickLookAudioHeight})
                s->remove(QString::fromLatin1(k));
        });
        QDir().mkpath(p(QStringLiteral("drag")));
        QImage img(200, 100, QImage::Format_RGB32);
        img.fill(Qt::darkCyan);
        QVERIFY(img.save(p(QStringLiteral("drag/a.png"))));
        QVERIFY(img.scaled(100, 100).save(p(QStringLiteral("drag/b.png"))));
        write(QStringLiteral("drag/note.txt"), "hello\n");
        write(QStringLiteral("drag/tone.mp3"), QByteArray(256, '\0'));
        tab()->navigate(p(QStringLiteral("drag")));
        tab()->setMode(BrowserTab::List, true);
        QTRY_COMPARE(tab()->itemCount(), 4);
        auto open = [&](const QString &name) -> QuickLookWindow * {
            m_win->activateWindow();
            QTest::qWaitForWindowActive(m_win);
            tab()->focusView();
            tab()->selectPaths({p(QStringLiteral("drag/") + name)});
            if (tab()->selectedPaths().size() != 1)
                return nullptr;
            key(Qt::Key_Space);
            auto *ql = m_win->findChild<QuickLookWindow *>();
            return ql && QTest::qWaitFor([ql] { return ql->isVisible(); }) ? ql : nullptr;
        };
        auto closeIt = [&](QuickLookWindow *ql) {
            QTest::keyClick(ql, Qt::Key_Escape);
            QTRY_VERIFY(!ql->isVisible());
        };

        // Image: the picture follows the window; its scale is remembered for the next image.
        QuickLookWindow *ql = open(QStringLiteral("a.png"));
        QVERIFY(ql);
        ZoomArea *area = ql->findChild<ZoomArea *>();
        QTRY_COMPARE(area->widget()->size(), QSize(200, 100));
        ql->resize(ql->size() * 1.5); // the user drags the corner: the picture grows as much, 150%
        QTRY_COMPARE(area->widget()->size(), QSize(300, 150));
        QTRY_COMPARE(s->value(Settings::QuickLookScale).toInt(), 150);
        closeIt(ql);
        QVERIFY(open(QStringLiteral("b.png")));
        QTRY_COMPARE(area->widget()->size(), QSize(150, 150));
        closeIt(ql);

        // Document: the window size, not the text.
        QVERIFY(open(QStringLiteral("note.txt")));
        auto *text = ql->findChild<QPlainTextEdit *>();
        QTRY_VERIFY(text && text->isVisible());
        const qreal font = text->font().pointSizeF();
        const QSize doc = ql->size() - QSize(120, 100);
        ql->resize(doc);
        QTRY_COMPARE(s->value(Settings::QuickLookDocWidth).toInt(), doc.width());
        QCOMPARE(s->value(Settings::QuickLookDocHeight).toInt(), doc.height());
        QCOMPARE(s->value(Settings::QuickLookDocScale).toInt(), 100);
        closeIt(ql);
        QVERIFY(open(QStringLiteral("note.txt")));
        QTRY_COMPARE(ql->size(), doc);
        QCOMPARE(text->font().pointSizeF(), font);
        closeIt(ql);

        // Sound: its own window size, by dragging or + / −.
        QVERIFY(open(QStringLiteral("tone.mp3")));
        QTRY_VERIFY(ql->preview()->isAudio());
        const QSize sound(600, 320);
        ql->resize(sound);
        QTRY_COMPARE(s->value(Settings::QuickLookAudioWidth).toInt(), sound.width());
        QCOMPARE(s->value(Settings::QuickLookAudioHeight).toInt(), sound.height());
        QCOMPARE(s->value(Settings::QuickLookDocWidth).toInt(), doc.width()); // documents keep theirs
        closeIt(ql);
        QVERIFY(open(QStringLiteral("tone.mp3")));
        QTRY_COMPARE(ql->size(), sound);
        QTest::keyClick(ql, Qt::Key_Equal); // +
        QTRY_COMPARE(s->value(Settings::QuickLookAudioWidth).toInt(), qMin(750, int(ql->screen()->availableGeometry().width() * 0.9)));
        QCOMPARE(s->value(Settings::QuickLookDocScale).toInt(), 100); // not the documents' scale
        closeIt(ql);
        tab()->navigate(m_tmp.path());
    }

    void quickLookTextSizeIsRemembered()
    {
        write(QStringLiteral("ql.txt"), "hello\n");
        Settings *s = Settings::instance();
        s->setValue(Settings::QuickLookDocScale, 100);
        tab()->navigate(m_tmp.path());
        tab()->focusView();
        tab()->selectPaths({p("ql.txt")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_Space);
        auto *ql = m_win->findChild<QuickLookWindow *>();
        QTRY_VERIFY(ql && ql->isVisible());
        auto *text = ql->findChild<QPlainTextEdit *>();
        QTRY_VERIFY(text && text->isVisible());
        const qreal base = s->value(Settings::PreviewDocFontSize).toInt();
        QCOMPARE(text->font().pointSizeF(), base);
        const QSize before = ql->size();
        QTest::keyClick(ql, Qt::Key_Minus, Qt::ControlModifier); // ⌘−: text and window, remembered
        QCOMPARE(s->value(Settings::QuickLookDocScale).toInt(), 80);
        QTRY_COMPARE(text->font().pointSizeF(), base * 0.8);
        QVERIFY(ql->width() <= before.width());
        QTest::keyClick(ql, Qt::Key_Equal); // +
        QTest::keyClick(ql, Qt::Key_Equal);
        QCOMPARE(s->value(Settings::QuickLookDocScale).toInt(), 125); // grows past a capped window
        QTRY_COMPARE(text->font().pointSizeF(), base * 1.25);
        const QRect avail = ql->screen()->availableGeometry();
        QVERIFY(ql->width() <= avail.width() * 9 / 10 + 1);
        QTest::keyClick(ql, Qt::Key_Escape);
        QTRY_VERIFY(!ql->isVisible());
        s->setValue(Settings::QuickLookDocScale, 100);
        QFile::remove(p("ql.txt"));
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
    }

    void leftAndRightMoveBetweenListAndSidebar()
    {
        tab()->focusView();
        tab()->selectPaths({p("Alpha")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        QVERIFY(tree && !tree->isExpanded(tree->currentIndex()));
        key(Qt::Key_Left); // nothing to fold, already at the top level
        QTRY_VERIFY(qobject_cast<Sidebar *>(focus()));
        QCOMPARE(tab()->path(), m_tmp.path()); // the sidebar got the keyboard, nothing navigated
        key(Qt::Key_Right);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));
        QCOMPARE(tab()->selectedPaths(), QStringList{p("Alpha")});
    }

    void sidebarKeepsCursorAndHighlightsOnlyOpenFolder()
    {
        auto *sidebar = m_win->findChild<Sidebar *>();
        QVERIFY(sidebar);
        const QStringList favorites = Sidebar::favorites();
        const QString originalPath = tab()->path();
        const auto restore = qScopeGuard([&] {
            tab()->navigate(originalPath);
            Sidebar::setFavorites(favorites);
            sidebar->insertFavorites({}, 0);
            tab()->setMode(BrowserTab::List, true);
            tab()->focusView();
        });
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QString a = fixture.filePath(QStringLiteral("A")), b = fixture.filePath(QStringLiteral("B"));
        QDir().mkpath(a);
        QDir().mkpath(b);
        QFile file(QDir(a).filePath(QStringLiteral("file.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("sample");
        file.close();
        sidebar->insertFavorites({a, b}, 0);
        auto *first = sidebar->topLevelItem(0)->child(0);
        auto *second = sidebar->topLevelItem(0)->child(1);
        tab()->setMode(BrowserTab::List, true);
        tab()->navigate(a);
        tab()->selectPaths({file.fileName()});
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{file.fileName()});
        sidebar->setCurrentItem(first, 0, QItemSelectionModel::NoUpdate);
        tab()->focusView();
        key(Qt::Key_Left, Qt::AltModifier);
        QTRY_VERIFY(sidebar->hasFocus());
        key(Qt::Key_Down);
        QCOMPARE(sidebar->currentItem(), second);
        QCOMPARE(sidebar->selectedItems(), QList<QTreeWidgetItem *>{first});
        QCOMPARE(tab()->path(), a);
        auto rowColor = [&](QTreeWidgetItem *item) {
            QImage image(QSize(250, 28), QImage::Format_ARGB32_Premultiplied);
            image.fill(Theme::colors().sidebarBg);
            QPainter painter(&image);
            QStyleOptionViewItem option;
            option.initFrom(sidebar);
            option.widget = sidebar;
            option.rect = image.rect();
            option.font = sidebar->font();
            option.fontMetrics = QFontMetrics(option.font);
            option.state = QStyle::State_Enabled; // hover and Qt selection do not define the open folder
            sidebar->itemDelegate()->paint(&painter, option, sidebar->indexFromItem(item));
            painter.end();
            return image.pixelColor(240, 14);
        };
        QCOMPARE(rowColor(first).rgba(), Theme::colors().selection.rgba());
        const QColor focused = rowColor(second);
        QVERIFY(focused != Theme::colors().sidebarBg);
        QVERIFY(focused != Theme::colors().selection);
        key(Qt::Key_Right, Qt::AltModifier);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));
        QCOMPARE(tab()->selectedPaths(), QStringList{file.fileName()});
        QCOMPARE(rowColor(second).rgba(), Theme::colors().sidebarBg.rgba());
        key(Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(sidebar->currentItem(), second);
        QCOMPARE(rowColor(second), focused);
        key(Qt::Key_Right); // the unmodified pane navigation preserves the same cursor too
        key(Qt::Key_Left);
        QTRY_COMPARE(sidebar->currentItem(), second);
        tab()->navigate(fixture.path());
        QVERIFY(sidebar->selectedItems().isEmpty());
        QCOMPARE(sidebar->currentItem(), second);
        QCOMPARE(rowColor(first).rgba(), Theme::colors().sidebarBg.rgba());
        sidebar->insertFavorites({}, 0); // rebuilding favorites retains the cursor by path
        second = sidebar->topLevelItem(0)->child(1);
        QCOMPARE(sidebar->currentItem(), second);
        QTest::keyClick(sidebar, Qt::Key_Return);
        QTRY_COMPARE(tab()->path(), b);
        QCOMPARE(sidebar->selectedItems(), QList<QTreeWidgetItem *>{second});
        QCOMPARE(rowColor(second).rgba(), Theme::colors().selection.rgba());
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

    void firstDownSelectsFirstItem()
    {
        auto *search = m_win->findChild<QLineEdit *>(QStringLiteral("search"));
        QVERIFY(search);
        for (BrowserTab::Mode mode : {BrowserTab::List, BrowserTab::Gallery}) {
            tab()->setMode(mode, true);
            QAbstractItemView *v = tab()->view();
            const QModelIndex first = v->model()->index(0, 0, v->rootIndex());
            const QModelIndex second = v->model()->index(1, 0, v->rootIndex());
            QVERIFY(first.isValid() && second.isValid());
            v->selectionModel()->setCurrentIndex(second, QItemSelectionModel::NoUpdate);
            v->selectionModel()->clearSelection();
            search->setFocus();
            key(Qt::Key_Down);
            QTRY_COMPARE(v->currentIndex(), first);
            QVERIFY(v->selectionModel()->isSelected(first));
            key(Qt::Key_Down); // only the next press advances
            if (mode == BrowserTab::List)
                QTRY_COMPARE(v->currentIndex(), second);
            v->selectionModel()->clearSelection();
            v->selectionModel()->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
            tab()->focusView();
            key(Qt::Key_Down);
            QTRY_COMPARE(v->currentIndex(), first);
            QVERIFY(v->selectionModel()->isSelected(first));
        }
        tab()->setMode(BrowserTab::List, true);
    }

    void contextualShortcutsCanBeChanged()
    {
        auto *sc = Shortcuts::instance();
        const QString rename = QStringLiteral("이름 변경"), quick = QStringLiteral("퀵 뷰어"), close = QStringLiteral("퀵 뷰어 닫기");
        const auto restore = qScopeGuard([=] {
            for (const QString &id : {rename, quick, close})
                Settings::instance()->remove(QStringLiteral("shortcuts/") + id);
        });
        sc->assign(rename, QKeySequence(QStringLiteral("F8")));
        sc->assign(quick, QKeySequence(QStringLiteral("F9")));
        sc->assign(close, QKeySequence(QStringLiteral("F7")));
        tab()->selectPaths({p("file1.go")});
        tab()->focusView();
        key(Qt::Key_F8);
        QTRY_VERIFY(qobject_cast<QLineEdit *>(focus()));
        key(Qt::Key_Escape);
        tab()->focusView();
        key(Qt::Key_F9);
        auto *ql = m_win->findChild<QuickLookWindow *>();
        QTRY_VERIFY(ql && ql->isVisible());
        QTest::keyClick(ql, Qt::Key_Space); // old preview close key is disabled
        QVERIFY(ql->isVisible());
        QTest::keyClick(ql, Qt::Key_F7);
        QTRY_VERIFY(!ql->isVisible());
    }

    void altArrowsMoveBetweenPanes()
    {
        auto *sidebar = m_win->findChild<Sidebar *>();
        auto *showSidebar = m_win->findChild<QAction *>(QStringLiteral("사이드바 보기"));
        QVERIFY(sidebar && showSidebar);
        const QString folder = tab()->path();
        for (BrowserTab::Mode mode : {BrowserTab::List, BrowserTab::Gallery, BrowserTab::Columns}) {
            tab()->setMode(mode, true);
            tab()->selectPaths({p("file1.go")});
            QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("file1.go")});
            tab()->focusView();
            if (showSidebar->isChecked())
                showSidebar->trigger();
            key(Qt::Key_Left, Qt::AltModifier);
            QTRY_VERIFY(sidebar->hasFocus() && sidebar->isVisible());
            QVERIFY(showSidebar->isChecked());
            QCOMPARE(tab()->path(), folder);
            key(Qt::Key_Right, Qt::AltModifier);
            QTRY_VERIFY2(tab()->view() == focus() || tab()->view()->isAncestorOf(focus()),
                         qPrintable(QStringLiteral("mode=%1 focus=%2/%3 keys=%4").arg(int(mode)).arg(QString::fromLatin1(focus()->metaObject()->className()), focus()->objectName(),
                                    Shortcuts::toStrings(Shortcuts::instance()->keys(QStringLiteral("파일뷰로 이동"))).join(QLatin1Char(',')))));
            QCOMPARE(tab()->selectedPaths(), QStringList{p("file1.go")});
        }
        tab()->setMode(BrowserTab::List, true);
        key(Qt::Key_Down, Qt::AltModifier); // unfolds without executing anything
        QPointer<TerminalWidget> terminal = qobject_cast<TerminalWidget *>(focus());
        QTRY_VERIFY(terminal && terminal->isVisible());
        QTRY_VERIFY2_WITH_TIMEOUT(terminal->isReady(), qPrintable(terminal->screenText()), 15000);
        QTest::keyClicks(terminal, "echo keep_this_line");
        QTRY_VERIFY(terminal->screenText().contains(QStringLiteral("keep_this_line")));
        const QString screen = terminal->screenText();
        const int tabs = m_win->findChild<QTabBar *>(QStringLiteral("termTabs"))->count();
        key(Qt::Key_Up, Qt::AltModifier);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));
        key(Qt::Key_Down, Qt::AltModifier);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(terminal));
        key(Qt::Key_Left, Qt::AltModifier); // app navigation wins over shell word movement
        QTRY_VERIFY(sidebar->hasFocus());
        key(Qt::Key_Down, Qt::AltModifier);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(terminal));
        key(Qt::Key_Right, Qt::AltModifier);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));
        QCOMPARE(tab()->path(), folder);
        QCOMPARE(m_win->findChild<QTabBar *>(QStringLiteral("termTabs"))->count(), tabs);
        QCOMPARE(terminal->screenText(), screen);
        // Reassigned pane keys must also pass through the terminal's shortcut override.
        auto *shortcuts = Shortcuts::instance();
        const QString id = QStringLiteral("파일뷰로 이동");
        const auto restore = qScopeGuard([=] { Settings::instance()->remove(QStringLiteral("shortcuts/") + id); });
        shortcuts->assign(id, QKeySequence(QStringLiteral("Alt+F8")));
        key(Qt::Key_Down, Qt::AltModifier);
        key(Qt::Key_F8, Qt::AltModifier);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));
    }

    void fontSizeKeys()
    {
        QAbstractItemView *v = tab()->view();
        const int base = QApplication::font().pointSize();
        const int rowBefore = v->sizeHintForRow(0);
        key(Qt::Key_Equal, Qt::ControlModifier); // ⌘+
        QTRY_COMPARE(v->font().pointSize(), base + 1);
        QCOMPARE(Settings::instance()->value(Settings::FontSize).toInt(), base + 1);
        key(Qt::Key_Equal, Qt::ControlModifier);
        key(Qt::Key_Equal, Qt::ControlModifier);
        QTRY_VERIFY(v->sizeHintForRow(0) > rowBefore); // rows grow with the text
        key(Qt::Key_Minus, Qt::ControlModifier); // ⌘−
        QTRY_COMPARE(v->font().pointSize(), base + 2);
        key(Qt::Key_0, Qt::ControlModifier); // ⌘0: system size
        QTRY_COMPARE(v->font().pointSize(), base);
        QCOMPARE(Settings::instance()->value(Settings::FontSize).toInt(), 0);
        QCOMPARE(v->sizeHintForRow(0), rowBefore);
    }

    void translationsLoad()
    {
        // Korean is the source text; the other languages come from the committed .qm files, which
        // must cover every message (scripts/i18n.sh). Checked without installing them app-wide.
        const QString dir = QStringLiteral(GIFILES_SOURCE_DIR "/i18n");
        const QList<QPair<QString, QString>> open = {{QStringLiteral("en"), QStringLiteral("Open")},
                                                     {QStringLiteral("ja"), QString()},
                                                     {QStringLiteral("zh_CN"), QString()}};
        for (const auto &[lang, expected] : open) {
            QTranslator tr;
            QVERIFY2(tr.load(QStringLiteral("gifiles_%1").arg(lang), dir), qPrintable(lang));
            const QString t = tr.translate("Gifiles", "열기");
            QVERIFY2(!t.isEmpty() && t != QStringLiteral("열기"), qPrintable(lang));
            if (!expected.isEmpty())
                QCOMPARE(t, expected);
            QVERIFY(!tr.translate("Gifiles", "선택한 항목들로…").isEmpty());
        }
        for (const auto &[lang, expected] : open) {
            QFile ts(dir + QStringLiteral("/gifiles_%1.ts").arg(lang));
            QVERIFY(ts.open(QIODevice::ReadOnly));
            QVERIFY2(!ts.readAll().contains("type=\"unfinished\""), qPrintable(QStringLiteral("untranslated messages in %1").arg(ts.fileName())));
        }
    }

    void terminalAddButtonOnlyWhenOpen()
    {
        QToolButton *add = nullptr;
        for (QToolButton *b : m_win->findChildren<QToolButton *>())
            if (b->toolTip().startsWith(QStringLiteral("새로운 터미널 탭")))
                add = b;
        QVERIFY(add);
        QAction *toggle = nullptr;
        for (QAction *a : m_win->actions())
            if (a->text() == QStringLiteral("터미널 펼치기") || a->text() == QStringLiteral("터미널 접기"))
                toggle = a;
        QVERIFY(toggle);
        if (toggle->isChecked())
            toggle->trigger(); // fold
        QTRY_VERIFY(!add->isVisible());
        toggle->trigger();
        QTRY_VERIFY(add->isVisible());
        toggle->trigger();
        QTRY_VERIFY(!add->isVisible());
        tab()->focusView();
    }

    void listSortsByEveryColumnBothWays()
    {
        // A header click sorts by that column, a second click turns the direction around. Folders stay
        // first either way (view.folders_first); names sort naturally and ignore case.
        Settings *st = Settings::instance();
        const auto restore = qScopeGuard([this, st] {
            st->remove(Settings::FoldersFirst);
            if (auto *tree = qobject_cast<QTreeView *>(tab()->view()))
                tree->sortByColumn(ColName, Qt::AscendingOrder);
            tab()->navigate(m_tmp.path());
            QDir(p("sorting")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("sorting")));
        QVERIFY(QDir().mkpath(p("sorting/Zeta")));
        QVERIFY(QDir().mkpath(p("sorting/beta")));
        // name, size, minutes old: every column gives a different order
        const struct { const char *name; int size; int age; } files[] = {
            {"file10.txt", 300, 30}, {"file2.txt", 10, 10}, {"Alpha.md", 2000, 40}, {"photo.png", 50, 20}};
        const QDateTime now = QDateTime::currentDateTime();
        for (const auto &f : files) {
            const QString rel = QStringLiteral("sorting/") + QLatin1String(f.name);
            write(rel, QByteArray(f.size, 'x'));
            QFile file(p(rel));
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(now.addSecs(-60 * f.age), QFileDevice::FileModificationTime));
        }
        tab()->navigate(p("sorting"));
        tab()->setMode(BrowserTab::List, true);
        QTRY_COMPARE(tab()->itemCount(), 6);
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        QVERIFY(tree);
        QHeaderView *header = tree->header();
        auto clickHeader = [&](int column) {
            const QPoint at(header->sectionViewportPosition(column) + header->sectionSize(column) / 2, header->height() / 2);
            QTest::mouseClick(header->viewport(), Qt::LeftButton, {}, at);
        };
        auto inOrder = [](QStringList names, Qt::SortOrder order) {
            if (order == Qt::DescendingOrder)
                std::reverse(names.begin(), names.end());
            return names;
        };
        const struct { int column; QStringList files; } expected[] = {
            {ColName, {"Alpha.md", "file2.txt", "file10.txt", "photo.png"}},
            {ColSize, {"file2.txt", "photo.png", "file10.txt", "Alpha.md"}},
            {ColDate, {"Alpha.md", "file10.txt", "photo.png", "file2.txt"}}, // oldest first
        };
        for (const auto &e : expected) {
            if (header->sortIndicatorSection() != e.column)
                clickHeader(e.column);
            QTRY_COMPARE(header->sortIndicatorSection(), e.column);
            for (int round = 0; round < 2; ++round) {
                const Qt::SortOrder order = header->sortIndicatorOrder();
                const QStringList shown = shownNames();
                QVERIFY2(shown.mid(0, 2).contains(QStringLiteral("beta")) && shown.mid(0, 2).contains(QStringLiteral("Zeta")),
                         qPrintable(shown.join(QLatin1Char(','))));
                QCOMPARE(shown.mid(2), inOrder(e.files, order));
                if (e.column != ColDate) // two folders made in the same second: by name
                    QCOMPARE(shown.mid(0, 2), inOrder({QStringLiteral("beta"), QStringLiteral("Zeta")}, order));
                clickHeader(e.column);
                QTRY_VERIFY(header->sortIndicatorOrder() != order);
            }
        }
        // Kind: grouped by kind (the two text files together), each group by name.
        clickHeader(ColKind);
        QTRY_COMPARE(header->sortIndicatorSection(), int(ColKind));
        if (header->sortIndicatorOrder() != Qt::AscendingOrder)
            clickHeader(ColKind);
        QTRY_COMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
        const QStringList byKind = shownNames().mid(2);
        QCOMPARE(byKind.indexOf(QStringLiteral("file10.txt")), byKind.indexOf(QStringLiteral("file2.txt")) + 1);
        // Folders among the files when folders_first is off.
        tree->sortByColumn(ColName, Qt::AscendingOrder);
        st->setValue(Settings::FoldersFirst, false);
        QTRY_COMPARE(shownNames(), (QStringList{"Alpha.md", "beta", "file2.txt", "file10.txt", "photo.png", "Zeta"}));
        tree->sortByColumn(ColName, Qt::DescendingOrder);
        QTRY_COMPARE(shownNames(), (QStringList{"Zeta", "photo.png", "file10.txt", "file2.txt", "beta", "Alpha.md"}));
    }

    void searchKeepsFilteringAcrossFolders()
    {
        // ⌘F: the name filter ignores case and applies to the folder shown, also after going elsewhere;
        // ↓ moves to the first match, Esc clears it.
        auto *search = m_win->findChild<QLineEdit *>(QStringLiteral("search"));
        QVERIFY(search);
        const auto restore = qScopeGuard([this, search] {
            search->clear();
            tab()->navigate(m_tmp.path());
        });
        key(Qt::Key_F, Qt::ControlModifier);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(search));
        type("ALP");
        QTRY_COMPARE(shownNames(), QStringList{QStringLiteral("Alpha")});
        key(Qt::Key_Down);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Alpha")});
        // The filter belongs to the shown folder's items only, and follows the browser.
        search->setText(QStringLiteral("deep"));
        QTRY_COMPARE(tab()->itemCount(), 0);
        tab()->navigate(p("Beta/inner"));
        QTRY_COMPARE(shownNames(), QStringList{QStringLiteral("deep.txt")});
        search->setText(QStringLiteral("nothing-like-this"));
        QTRY_COMPARE(tab()->itemCount(), 0);
        search->setFocus();
        key(Qt::Key_Escape);
        QTRY_COMPARE(search->text(), QString());
        QTRY_COMPARE(tab()->itemCount(), 1);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));
    }

    void hiddenFilesToggleWithShortcut()
    {
        // ⇧⌘. shows hidden files in every tab and remembers it in config.toml; again hides them.
        QVERIFY(!App::instance()->showHidden());
        const auto restore = qScopeGuard([this] {
            App::instance()->setShowHidden(false);
            tab()->navigate(m_tmp.path());
            QDir(p("hidden")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("hidden")));
        write(QStringLiteral("hidden/shown.txt"), "x");
        write(QStringLiteral("hidden/.secret"), "x");
#ifdef Q_OS_WIN
        QVERIFY(QProcess::execute(QStringLiteral("attrib"), {QStringLiteral("+h"), np("hidden/.secret")}) == 0);
#endif
        tab()->navigate(p("hidden"));
        QTRY_COMPARE(tab()->itemCount(), 1);
        tab()->focusView();
        key(Qt::Key_Period, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(tab()->itemCount(), 2);
        QVERIFY(App::instance()->showHidden());
        QVERIFY(Settings::instance()->flag(Settings::ShowHidden));
        QVERIFY(m_win->findChild<QAction *>(QStringLiteral("숨김 파일 보기"))->isChecked());
        key(Qt::Key_Period, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(tab()->itemCount(), 1);
        QVERIFY(!Settings::instance()->flag(Settings::ShowHidden));
    }

    void dropOnFolderRowMovesIntoItAndUndoes()
    {
        // Dropping files on a folder in the list moves them there (same volume, like Finder); a folder
        // can't go into itself; a drop on a file row lands in the shown folder and loses nothing.
        const auto restore = qScopeGuard([this] {
            tab()->navigate(m_tmp.path());
            QDir(p("dropping")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("dropping")));
        QVERIFY(QDir().mkpath(p("dropping/target")));
        write(QStringLiteral("dropping/moved.txt"), "move me");
        write(QStringLiteral("dropping/other.txt"), "stay");
        tab()->navigate(p("dropping"));
        tab()->setMode(BrowserTab::List, true);
        QTRY_COMPARE(tab()->itemCount(), 3);
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        auto *proxy = static_cast<FileProxy *>(tree->model());
        auto dropAt = [&](const QString &onto, const QStringList &paths) {
            QMimeData md;
            QList<QUrl> urls;
            for (const QString &x : paths)
                urls << QUrl::fromLocalFile(x);
            md.setUrls(urls);
            const QPoint at = tree->visualRect(proxy->indexForPath(onto)).center();
            QDragEnterEvent enter(at, Qt::CopyAction | Qt::MoveAction, &md, Qt::LeftButton, {});
            QApplication::sendEvent(tree->viewport(), &enter);
            QDragMoveEvent move(at, Qt::CopyAction | Qt::MoveAction, &md, Qt::LeftButton, {});
            QApplication::sendEvent(tree->viewport(), &move);
            const bool accepted = move.isAccepted();
            QDropEvent drop(QPointF(at), Qt::CopyAction | Qt::MoveAction, &md, Qt::LeftButton, {});
            QApplication::sendEvent(tree->viewport(), &drop);
            return accepted;
        };
        QVERIFY(dropAt(p("dropping/target"), {p("dropping/moved.txt")}));
        QTRY_VERIFY(QFile::exists(p("dropping/target/moved.txt")));
        QTRY_VERIFY(!QFile::exists(p("dropping/moved.txt")));
        QCOMPARE(readFile(QStringLiteral("dropping/target/moved.txt")), QStringLiteral("move me"));
        QTRY_VERIFY(App::instance()->canUndo());
        tab()->focusView();
        key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(p("dropping/moved.txt")));
        QVERIFY(!QFile::exists(p("dropping/target/moved.txt")));

        // ⌘Z above leaves Ctrl as the last known modifier offscreen, and Ctrl means copy on a drop
        // on Windows/Linux: release it first.
        QTest::keyRelease(tree->viewport(), Qt::Key_Control, Qt::NoModifier);
        QTRY_COMPARE(QGuiApplication::queryKeyboardModifiers(), Qt::NoModifier);
        QVERIFY(!dropAt(p("dropping/target"), {p("dropping/target")})); // not into itself
        dropAt(p("dropping/other.txt"), {p("dropping/moved.txt")});      // same folder: nothing moves
        QTest::qWait(300);
        QCOMPARE(QDir(p("dropping")).entryList(QDir::Files), (QStringList{"moved.txt", "other.txt"}));
        QCOMPARE(readFile(QStringLiteral("dropping/other.txt")), QStringLiteral("stay"));
        QVERIFY(QDir(p("dropping/target")).isEmpty());
    }

    void backSelectsTheFolderYouCameFrom()
    {
        tab()->navigate(m_tmp.path());
        tab()->navigate(p("Beta/inner"));
        QTRY_COMPARE(tab()->itemCount(), 1);
        tab()->focusView();
        key(Qt::Key_BracketLeft, Qt::ControlModifier); // ⌘[
        QTRY_COMPARE(tab()->path(), m_tmp.path());
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Beta")}); // the way back in is marked
        QVERIFY(m_win->findChild<QAction *>(QStringLiteral("앞으로"))->isEnabled());
        key(Qt::Key_BracketRight, Qt::ControlModifier); // ⌘]
        QTRY_COMPARE(tab()->path(), p("Beta/inner"));
        QVERIFY(!m_win->findChild<QAction *>(QStringLiteral("앞으로"))->isEnabled());
        key(Qt::Key_Up, Qt::ControlModifier);
        QTRY_COMPARE(tab()->path(), p("Beta"));
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("Beta/inner")});
        tab()->navigate(m_tmp.path());
    }

    void openFolderSurvivesBeingReplaced()
    {
        // The open folder is renamed away and back from outside (a terminal, git, a build tool):
        // the list must keep showing it, not lose its root and show the whole disk from "/".
        const auto restore = qScopeGuard([this] {
            tab()->navigate(m_tmp.path());
            QDir(p("bounce")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("bounce")));
        QVERIFY(QDir().mkpath(p("bounce/sub")));
        write(QStringLiteral("bounce/sub/x.txt"), "x");
        tab()->navigate(p("bounce"));
        tab()->setMode(BrowserTab::List, true);
        QTRY_COMPARE(tab()->itemCount(), 1);
        tab()->navigate(p("bounce/sub"));
        QTRY_COMPARE(tab()->itemCount(), 1);
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        auto *proxy = static_cast<FileProxy *>(tree->model());
        QCOMPARE(proxy->filePath(tree->rootIndex()), p("bounce/sub"));
        QVERIFY(QDir().rename(p("bounce/sub"), p("bounce/sub2")));
        QTest::qWait(1000); // the watcher reports it
        QVERIFY(QDir().rename(p("bounce/sub2"), p("bounce/sub")));
        QTest::qWait(1000);
        QTRY_COMPARE(tab()->itemCount(), 1);
        QCOMPARE(shownNames(), QStringList{QStringLiteral("x.txt")});
    }

    void sidebarFavoritesByMouseDropAndMenu()
    {
        // Favorites: a click opens one; files dropped between rows become favorites there; a dragged
        // favorite moves; files dropped on the middle of a folder favorite go into that folder;
        // the context menu opens one in a tab or removes it. Missing ones are kept, hidden, at the end.
        auto *sb = m_win->findChild<Sidebar *>();
        QVERIFY(sb);
        const QStringList before = Sidebar::favorites();
        const auto restore = qScopeGuard([this, before] {
            Sidebar::setFavorites(before);
            emit Theme::instance()->changed(); // rebuilds the sidebar (it doesn't follow the setting itself)
            tab()->navigate(m_tmp.path());
            QDir(p("fav")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("fav")));
        for (const char *d : {"fav/A", "fav/B", "fav/C"})
            QVERIFY(QDir().mkpath(p(QString::fromLatin1(d))));
        write(QStringLiteral("fav/note.txt"), "note");
        const QString A = p("fav/A"), B = p("fav/B"), C = p("fav/C"), missing = p("fav/gone");
        Sidebar::setFavorites({A, missing, B});
        emit Theme::instance()->changed();
        auto shown = [&] { // every change rebuilds the items: look them up each time
            QTreeWidgetItem *favorites = sb->topLevelItem(0);
            QStringList out;
            for (int i = 0; i < favorites->childCount(); ++i)
                out << favorites->child(i)->toolTip(0);
            return out;
        };
        auto native = [](const QStringList &l) {
            QStringList out;
            for (const QString &x : l)
                out << QDir::toNativeSeparators(x);
            return out;
        };
        QCOMPARE(shown(), native({A, B})); // the missing one isn't shown
        auto rowOf = [&](const QString &path) {
            QTreeWidgetItem *favorites = sb->topLevelItem(0);
            for (int i = 0; i < favorites->childCount(); ++i)
                if (favorites->child(i)->toolTip(0) == QDir::toNativeSeparators(path))
                    return sb->visualItemRect(favorites->child(i));
            return QRect();
        };

        // A click opens the folder in the list and gives it the keyboard.
        QTest::mouseClick(sb->viewport(), Qt::LeftButton, {}, rowOf(B).center());
        QTRY_COMPARE(tab()->path(), B);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));

        auto drag = [&](const QPoint &at, QMimeData *md) {
            QDragEnterEvent enter(at, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, md, Qt::LeftButton, {});
            QApplication::sendEvent(sb->viewport(), &enter);
            QDragMoveEvent move(at, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, md, Qt::LeftButton, {});
            QApplication::sendEvent(sb->viewport(), &move);
            const Qt::DropAction action = move.isAccepted() ? move.dropAction() : Qt::IgnoreAction;
            if (action != Qt::IgnoreAction) {
                (void)sb->viewport()->grab(); // paints the insertion line
                QDropEvent drop(QPointF(at), Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, md, Qt::LeftButton, {});
                QApplication::sendEvent(sb->viewport(), &drop);
            } else {
                QDragLeaveEvent leave;
                QApplication::sendEvent(sb->viewport(), &leave);
            }
            return action;
        };
        // A folder from outside dropped on the top edge of B goes in before B.
        {
            QMimeData md;
            md.setUrls({QUrl::fromLocalFile(C)});
            QCOMPARE(drag(rowOf(B).topLeft() + QPoint(40, 2), &md), Qt::LinkAction);
            QTRY_COMPARE(shown(), native({A, C, B}));
            QCOMPARE(Sidebar::favorites(), (QStringList{A, C, B, missing})); // the missing one is kept at the end
        }
        // A favorite dragged onto the top half of A moves there.
        {
            QMimeData md;
            md.setData(QStringLiteral("application/x-gifiles-favorite"), B.toUtf8());
            QCOMPARE(drag(rowOf(A).topLeft() + QPoint(40, rowOf(A).height() / 2 - 2), &md), Qt::MoveAction);
            QTRY_COMPARE(shown(), native({B, A, C}));
            // ... but not onto a volume.
            QTreeWidgetItem *volume = sb->topLevelItem(1)->child(0);
            QVERIFY(volume);
            QCOMPARE(drag(sb->visualItemRect(volume).center(), &md), Qt::IgnoreAction);
            QCOMPARE(shown(), native({B, A, C}));
        }
        // A file dropped on the middle of folder A goes into A (a move on the same volume), undoable.
        {
            QMimeData md;
            md.setUrls({QUrl::fromLocalFile(p("fav/note.txt"))});
            QVERIFY(drag(rowOf(A).center(), &md) != Qt::IgnoreAction);
            QTRY_VERIFY(QFile::exists(p("fav/A/note.txt")));
            QVERIFY(!QFile::exists(p("fav/note.txt")));
            QCOMPARE(shown(), native({B, A, C})); // not a new favorite
            QTRY_VERIFY(App::instance()->canUndo());
            tab()->focusView();
            key(Qt::Key_Z, Qt::ControlModifier);
            QTRY_VERIFY(QFile::exists(p("fav/note.txt")));
        }
        // A file dropped below everything becomes the last favorite; opening it shows it in its folder.
        {
            QMimeData md;
            md.setUrls({QUrl::fromLocalFile(p("fav/note.txt"))});
            QCOMPARE(drag(QPoint(40, sb->viewport()->height() - 3), &md), Qt::LinkAction);
            QTRY_COMPARE(shown(), native({B, A, C, p("fav/note.txt")}));
            QTest::mouseClick(sb->viewport(), Qt::LeftButton, {}, rowOf(p("fav/note.txt")).center());
            QTRY_COMPARE(tab()->path(), p("fav"));
            QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("fav/note.txt")});
        }
        // Context menu: open in a new tab, remove from the sidebar.
        auto *tabs = m_win->findChild<QTabWidget *>();
        const int tabCount = tabs->count();
        auto menuOn = [&](const QString &path, const QString &item) {
            bool found = false;
            onNextPopup([&](QWidget *w) {
                if (auto *menu = qobject_cast<QMenu *>(w)) {
                    for (QAction *a : menu->actions())
                        if (a->text() == item) {
                            found = true;
                            a->trigger();
                        }
                    menu->close();
                }
            });
            const QPoint at = rowOf(path).center();
            QContextMenuEvent e(QContextMenuEvent::Mouse, at, sb->viewport()->mapToGlobal(at));
            QApplication::sendEvent(sb->viewport(), &e);
            return found;
        };
        QVERIFY(menuOn(C, QStringLiteral("새로운 탭에서 열기")));
        QTRY_COMPARE(tabs->count(), tabCount + 1);
        QCOMPARE(tab()->path(), C);
        tabs->tabCloseRequested(tabs->currentIndex());
        QTRY_COMPARE(tabs->count(), tabCount);
        QVERIFY(menuOn(A, QStringLiteral("사이드바에서 제거")));
        QTRY_COMPARE(shown(), native({B, C, p("fav/note.txt")}));
        QVERIFY(!Sidebar::favorites().contains(A));
        QVERIFY(QDir(A).exists()); // only the favorite went away
    }

    void sidebarKeyboardOpensPlacesAndVolumes()
    {
        // ⌥← puts the keyboard in the sidebar; ↑/↓ move without opening; Return opens; → goes back.
        auto *sb = m_win->findChild<Sidebar *>();
        QVERIFY(sb);
        const QStringList before = Sidebar::favorites();
        const auto restore = qScopeGuard([this, before] {
            Sidebar::setFavorites(before);
            emit Theme::instance()->changed();
            tab()->navigate(m_tmp.path());
            tab()->focusView();
        });
        Sidebar::setFavorites({p("Alpha"), p("Beta")});
        emit Theme::instance()->changed();
        tab()->navigate(p("Alpha"));
        tab()->focusView();
        key(Qt::Key_Left, Qt::AltModifier);
        QTRY_VERIFY(sb->hasFocus());
        QCOMPARE(sb->currentItem()->toolTip(0), np("Alpha")); // starts at the open folder
        key(Qt::Key_Down);
        QCOMPARE(sb->currentItem()->toolTip(0), np("Beta"));
        QCOMPARE(tab()->path(), p("Alpha")); // moving doesn't open
        key(Qt::Key_Return);
        QTRY_COMPARE(tab()->path(), p("Beta"));
        // Down again reaches the volumes ("위치"); Return opens the first one at its root.
        QTreeWidgetItem *volume = sb->topLevelItem(1)->child(0);
        QVERIFY(volume);
        const QString root = QDir::fromNativeSeparators(volume->toolTip(0));
        QVERIFY(QFileInfo(root).isDir());
        sb->setFocus();
        QTRY_VERIFY(sb->hasFocus());
        for (int i = 0; i < 3 && sb->currentItem() != volume; ++i)
            key(Qt::Key_Down);
        QCOMPARE(sb->currentItem(), volume);
        key(Qt::Key_Return);
        QTRY_COMPARE(QDir::cleanPath(tab()->path()), QDir::cleanPath(root));
        QTRY_VERIFY(sb->selectedItems() == QList<QTreeWidgetItem *>{volume}); // marked as the open place
        sb->setFocus();
        key(Qt::Key_Right);
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));
    }

    void pathBarCrumbsAndGoToFolder()
    {
        auto *bar = m_win->findChild<PathBar *>();
        QVERIFY(bar);
        QLineEdit *edit = bar->findChild<QLineEdit *>();
        QVERIFY(edit);
        tab()->navigate(p("Beta/inner"));
        // Breadcrumb: the last segment is the open folder; a click on another opens it.
        QToolButton *current = bar->findChild<QToolButton *>(QStringLiteral("crumbCurrent"));
        QVERIFY(current);
        QTRY_COMPARE(current->text(), QStringLiteral("inner"));
        QToolButton *beta = nullptr;
        for (QToolButton *b : bar->findChildren<QToolButton *>(QStringLiteral("crumb")))
            if (b->toolTip() == np("Beta"))
                beta = b;
        QVERIFY(beta && beta->isVisible());
        QTest::mouseClick(beta, Qt::LeftButton);
        QTRY_COMPARE(tab()->path(), p("Beta"));

        // ⇧⌘G turns it into a field with the folder's path (ending in a separator), cursor at the end.
        auto goTo = [&](const QString &text) {
            tab()->focusView();
            key(Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_VERIFY(edit->isVisible() && edit->hasFocus());
            edit->setText(text); // as typed, without the completer's popup in the way
            key(Qt::Key_Return);
        };
        tab()->focusView();
        key(Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(edit->isVisible() && edit->hasFocus());
        if (!util::isInside(p("Beta"), QDir::homePath()))
            QCOMPARE(edit->text(), np("Beta") + QDir::separator());
        QCOMPARE(edit->cursorPosition(), edit->text().size());
        key(Qt::Key_Escape); // cancel: nothing happens, the list gets the keyboard back
        QTRY_VERIFY(!edit->isVisible());
        QCOMPARE(tab()->path(), p("Beta"));
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));

        goTo(QStringLiteral("inner")); // relative to the open folder
        QTRY_COMPARE(tab()->path(), p("Beta/inner"));
        QVERIFY(!edit->isVisible());
        goTo(np("README.md")); // a file: its folder, with the file selected
        QTRY_COMPARE(tab()->path(), m_tmp.path());
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("README.md")});
        goTo(np("no such folder/really")); // nothing to open: stays
        QTest::qWait(100);
        QCOMPARE(tab()->path(), m_tmp.path());
        QTRY_VERIFY(!edit->isVisible());
        QTRY_COMPARE(focus(), static_cast<QWidget *>(tab()->view()));

        // A click on the empty end of the bar edits too; leaving the field cancels.
        QTest::mouseClick(bar, Qt::LeftButton, {}, QPoint(bar->width() - 4, bar->height() / 2));
        QTRY_VERIFY(edit->isVisible() && edit->hasFocus());
        tab()->focusView();
        QTRY_VERIFY(!edit->isVisible());
        QCOMPARE(tab()->path(), m_tmp.path());

#ifndef Q_OS_WIN // QDir::homePath() follows $HOME on Unix only
        // ~ is the home folder, both shown and typed.
        const QByteArray home = qgetenv("HOME");
        const auto restoreHome = qScopeGuard([home] { qputenv("HOME", home); });
        qputenv("HOME", m_tmp.path().toUtf8());
        tab()->navigate(p("Beta"));
        tab()->focusView();
        key(Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(edit->isVisible() && edit->hasFocus());
        QCOMPARE(edit->text(), QStringLiteral("~/Beta/"));
        key(Qt::Key_Escape);
        goTo(QStringLiteral("~/Alpha"));
        QTRY_COMPARE(tab()->path(), p("Alpha"));
        goTo(QStringLiteral("~"));
        QTRY_COMPARE(tab()->path(), m_tmp.path());
#endif
        tab()->navigate(m_tmp.path());
    }

    void sessionRestoresWindowsTabsAndModes()
    {
        // Quitting saves every window's tabs, their views and the current tab; the next start opens
        // them again. Tabs whose folder is gone are left out, and a window with none left too.
        auto mainWindows = [] {
            QList<MainWindow *> out;
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *mw = qobject_cast<MainWindow *>(w); mw && mw->isVisible())
                    out << mw;
            return out;
        };
        const QList<MainWindow *> before = mainWindows();
        const auto restore = qScopeGuard([this, before, mainWindows] {
            for (MainWindow *w : mainWindows())
                if (!before.contains(w))
                    w->close();
            QSettings().remove(QStringLiteral("session"));
            m_win->activateWindow();
            (void)QTest::qWaitForWindowActive(m_win);
        });
        QPointer<MainWindow> w2 = App::instance()->newWindow({p("Alpha"), p("Beta")}, m_win);
        QVERIFY(QTest::qWaitForWindowExposed(w2));
        auto *tabs2 = w2->findChild<QTabWidget *>();
        QCOMPARE(tabs2->count(), 2);
        tabs2->setCurrentIndex(1);
        w2->tab()->setMode(BrowserTab::Columns);
        tabs2->setCurrentIndex(0);
        w2->tab()->setMode(BrowserTab::List);
        tabs2->setCurrentIndex(1);
        App::instance()->saveSession({w2});
        QSettings s;
        QVariantList saved = s.value(QStringLiteral("session/windows")).toList();
        QCOMPARE(saved.size(), 1);
        QVariantMap first = saved.first().toMap();
        QCOMPARE(first.value(QStringLiteral("tabs")).toStringList(), (QStringList{p("Alpha"), p("Beta")}));
        QCOMPARE(first.value(QStringLiteral("current")).toInt(), 1);
        // A folder deleted since, and a window that had only such folders.
        first.insert(QStringLiteral("tabs"), QStringList{p("Alpha"), p("gone-away"), p("Beta")});
        first.insert(QStringLiteral("modes"), QVariantList{int(BrowserTab::List), int(BrowserTab::Gallery), int(BrowserTab::Columns)});
        first.insert(QStringLiteral("current"), 1);
        saved = {first, QVariantMap{{QStringLiteral("tabs"), QStringList{p("gone-too")}}}};
        s.setValue(QStringLiteral("session/windows"), saved);
        s.sync();
        w2->close();
        QTRY_VERIFY(!w2 || !w2->isVisible());

        App::instance()->restoreSession();
        QList<MainWindow *> opened;
        for (MainWindow *w : mainWindows())
            if (!before.contains(w))
                opened << w;
        QCOMPARE(opened.size(), 1);
        auto *tabs = opened.first()->findChild<QTabWidget *>();
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(static_cast<BrowserTab *>(tabs->widget(0))->path(), p("Alpha"));
        QCOMPARE(static_cast<BrowserTab *>(tabs->widget(1))->path(), p("Beta"));
        QCOMPARE(tabs->currentIndex(), 1);
        QCOMPARE(static_cast<BrowserTab *>(tabs->widget(0))->mode(), BrowserTab::List);
        QCOMPARE(static_cast<BrowserTab *>(tabs->widget(1))->mode(), BrowserTab::Columns);
    }

    void windowOpensItsTabsInOrder()
    {
        // A window made from several folders (a restored session, ⌘N) shows them in that order.
        QPointer<MainWindow> w2 = App::instance()->newWindow({p("Alpha"), p("Beta"), p("감마")}, m_win);
        const auto restore = qScopeGuard([this, w2] {
            if (w2)
                w2->close();
            m_win->activateWindow();
            (void)QTest::qWaitForWindowActive(m_win);
        });
        QVERIFY(QTest::qWaitForWindowExposed(w2));
        auto *tabs = w2->findChild<QTabWidget *>();
        QStringList paths;
        for (int i = 0; i < tabs->count(); ++i)
            paths << static_cast<BrowserTab *>(tabs->widget(i))->path();
        QCOMPARE(paths, (QStringList{p("Alpha"), p("Beta"), p("감마")}));
    }

    void tabKeysCycleAndLastTabClosesWindow()
    {
        QPointer<MainWindow> w2 = App::instance()->newWindow({p("Alpha"), p("Beta"), p("감마")}, m_win);
        const auto restore = qScopeGuard([this, w2] {
            if (w2)
                w2->close();
            m_win->activateWindow();
            (void)QTest::qWaitForWindowActive(m_win);
        });
        QVERIFY(QTest::qWaitForWindowExposed(w2));
        w2->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(w2));
        auto *tabs = w2->findChild<QTabWidget *>();
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->currentIndex(), 0);
        auto pathAt = [tabs](int i) { return static_cast<BrowserTab *>(tabs->widget(i))->path(); };
        w2->tab()->focusView();
        key(Qt::Key_Tab, kTerminalMods); // ⌃Tab: next tab
        QTRY_COMPARE(tabs->currentIndex(), 1);
        QCOMPARE(w2->tab()->path(), pathAt(1));
        QCOMPARE(tabs->tabText(1), util::displayName(pathAt(1)));
        QCOMPARE(w2->windowTitle(), util::displayName(pathAt(1)));
        key(Qt::Key_BraceRight, Qt::ControlModifier); // ⌘}
        QTRY_COMPARE(tabs->currentIndex(), 2);
        key(Qt::Key_Tab, kTerminalMods); // wraps around
        QTRY_COMPARE(tabs->currentIndex(), 0);
        key(Qt::Key_BraceLeft, Qt::ControlModifier); // ⌘{ back, wrapping
        QTRY_COMPARE(tabs->currentIndex(), 2);
        // ⌘W closes tabs; on the last one it closes the window, not the app.
        for (int n = 3; n > 1; --n) {
            w2->tab()->focusView();
            key(Qt::Key_W, Qt::ControlModifier);
            QTRY_COMPARE(tabs->count(), n - 1);
        }
        w2->tab()->focusView();
        key(Qt::Key_W, Qt::ControlModifier);
        QTRY_VERIFY(!w2 || !w2->isVisible());
        QVERIFY(m_win->isVisible());
    }

    void pasteConflictsAskAndReplaceIsUndoable()
    {
        // Pasting onto names already there asks: 둘 다 유지 / 건너뛰기 / 대치 / 중단. Replacing puts the old
        // item in the trash and ⌘Z brings it back: nothing is lost.
        const auto restore = qScopeGuard([this] {
            tab()->navigate(m_tmp.path());
            QDir(p("clash")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("clash")));
        QVERIFY(QDir().mkpath(p("clash/src")));
        QVERIFY(QDir().mkpath(p("clash/dst")));
        write(QStringLiteral("clash/src/a.txt"), "new a");
        write(QStringLiteral("clash/src/b.txt"), "new b");
        write(QStringLiteral("clash/dst/a.txt"), "old a");
        write(QStringLiteral("clash/dst/b.txt"), "old b");
        auto copyFromSrc = [&](const QStringList &names) {
            tab()->navigate(p("clash/src"));
            QStringList paths;
            for (const QString &n : names)
                paths << p(QStringLiteral("clash/src/") + n);
            tab()->selectPaths(paths);
            QTRY_COMPARE(tab()->selectedPaths().size(), names.size());
            m_win->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(m_win));
            tab()->focusView();
            key(Qt::Key_C, Qt::ControlModifier);
            tab()->navigate(p("clash/dst"));
            QTRY_COMPARE(tab()->path(), p("clash/dst"));
            tab()->focusView();
        };
        int boxes = 0;
        // Answers the conflict box(es) by button text; `all` ticks "나머지 … 항목에도 적용".
        auto answer = [&](const QString &button, bool all = false) {
            onNextPopup([&, button, all](QWidget *w) {
                auto *box = qobject_cast<QMessageBox *>(w);
                if (!box)
                    return;
                ++boxes;
                if (all && box->checkBox())
                    box->checkBox()->setChecked(true);
                if (QPushButton *b = buttonNamed(box, button))
                    b->click();
                else
                    box->reject();
            });
        };
        auto dstFiles = [&] { return QDir(p("clash/dst")).entryList(QDir::Files, QDir::Name); };
        auto pasteKey = [&] { // offscreen, a closed message box leaves no active window behind
            m_win->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(m_win));
            tab()->focusView();
            key(Qt::Key_V, Qt::ControlModifier);
        };

        copyFromSrc({QStringLiteral("a.txt")});
        answer(QStringLiteral("둘 다 유지"));
        pasteKey();
        // The copy is selected when the job is done (before that a temporary file can be in the
        // folder, and the copy can be there without its bytes).
        QTRY_VERIFY(tab()->selectedPaths().size() == 1 && tab()->selectedPaths().first() != p("clash/dst/a.txt") &&
                    tab()->selectedPaths().first().startsWith(p("clash/dst/")));
        const QString kept = QFileInfo(tab()->selectedPaths().first()).fileName();
        QCOMPARE(dstFiles().size(), 3);
        QVERIFY(dstFiles().contains(kept));
        QCOMPARE(boxes, 1);
        QCOMPARE(readFile(QStringLiteral("clash/dst/a.txt")), QStringLiteral("old a"));
        QCOMPARE(readFile(QStringLiteral("clash/dst/") + kept), QStringLiteral("new a"));
        QFile::remove(p(QStringLiteral("clash/dst/") + kept));

        answer(QStringLiteral("건너뛰기"));
        pasteKey();
        QTRY_COMPARE(boxes, 2);
        QTest::qWait(300);
        QCOMPARE(dstFiles(), (QStringList{"a.txt", "b.txt"}));
        QCOMPARE(readFile(QStringLiteral("clash/dst/a.txt")), QStringLiteral("old a"));

        answer(QStringLiteral("대치"));
        pasteKey();
        QTRY_COMPARE(readFile(QStringLiteral("clash/dst/a.txt")), QStringLiteral("new a"));
        QCOMPARE(boxes, 3);
        QTRY_VERIFY(App::instance()->canUndo());
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("clash/dst/a.txt")}); // the job's record is in
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
        tab()->focusView();
        key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(readFile(QStringLiteral("clash/dst/a.txt")), QStringLiteral("old a")); // back from the trash
        QCOMPARE(dstFiles(), (QStringList{"a.txt", "b.txt"}));
        QCOMPARE(readFile(QStringLiteral("clash/src/a.txt")), QStringLiteral("new a")); // a copy: the source stays

        // Two clashes, "apply to the rest" ticked: one question for both.
        copyFromSrc({QStringLiteral("a.txt"), QStringLiteral("b.txt")});
        answer(QStringLiteral("건너뛰기"), true);
        pasteKey();
        QTRY_COMPARE(boxes, 4);
        QTest::qWait(400);
        QCOMPARE(boxes, 4);
        QCOMPARE(readFile(QStringLiteral("clash/dst/b.txt")), QStringLiteral("old b"));
        // 중단 stops before anything happens, even for the items without a clash.
        write(QStringLiteral("clash/src/c.txt"), "new c");
        copyFromSrc({QStringLiteral("a.txt"), QStringLiteral("c.txt")});
        answer(QStringLiteral("중단"));
        pasteKey();
        QTRY_COMPARE(boxes, 5);
        QTest::qWait(300);
        QCOMPARE(dstFiles(), (QStringList{"a.txt", "b.txt"}));
    }

    void getInfoCopyPathAndPreviewPane()
    {
        // ⌘I: a window per item with kind, size and place (a folder's size is counted in the background).
        tab()->navigate(m_tmp.path());
        tab()->selectPaths({p("README.md"), p("Beta")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 2);
        tab()->focusView();
        key(Qt::Key_I, Qt::ControlModifier);
        auto infoFor = [this](const QString &name) -> QDialog * {
            for (QDialog *d : m_win->findChildren<QDialog *>())
                if (d->isVisible() && d->windowTitle() == Gifiles::tr("%1 정보").arg(name))
                    return d;
            return nullptr;
        };
        QPointer<QDialog> fileInfo, dirInfo;
        QTRY_VERIFY((fileInfo = infoFor(QStringLiteral("README.md"))) && (dirInfo = infoFor(QStringLiteral("Beta"))));
        QVERIFY(showsText(fileInfo, QStringLiteral("%1바이트").arg(QFileInfo(p("README.md")).size())));
        QVERIFY(showsText(fileInfo, QDir::toNativeSeparators(m_tmp.path())));
        QTRY_VERIFY(showsText(dirInfo, QStringLiteral("1개 파일"))); // Beta/inner/deep.txt
        fileInfo->close();
        dirInfo->close();
        QTRY_VERIFY(!fileInfo && !dirInfo); // they delete themselves
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));

        // ⌥⌘C: the selected paths as the system shows them, one per line; nothing selected → the folder.
        tab()->focusView();
        key(Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), np("Beta") + QLatin1Char('\n') + np("README.md"));
        tab()->view()->selectionModel()->clearSelection();
        key(Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QDir::toNativeSeparators(m_tmp.path()));

    }

    void previewPaneFollowsSelection()
    {
        // ⇧⌘P: the preview pane follows the selection, and shows the current item as it opens.
        PreviewWidget *pane = nullptr; // the window's own, not Quick Look's or a column view's
        for (PreviewWidget *pw : m_win->findChildren<PreviewWidget *>()) {
            bool inTab = false;
            for (QWidget *w = pw->parentWidget(); w; w = w->parentWidget())
                inTab = inTab || qobject_cast<BrowserTab *>(w) || qobject_cast<QuickLookWindow *>(w);
            if (!inTab)
                pane = pw;
        }
        QVERIFY(pane && !pane->isVisible());
        const auto hide = qScopeGuard([this, pane] {
            if (pane->isVisible())
                m_win->findChild<QAction *>(QStringLiteral("미리보기 보기"))->trigger();
        });
        tab()->selectPaths({p("README.md")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        tab()->focusView();
        key(Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(pane->isVisible());
        key(Qt::Key_Up);
        QTRY_VERIFY(!pane->path().isEmpty());
        QCOMPARE(pane->path(), tab()->selectedPaths().value(0));
        key(Qt::Key_Down);
        QTRY_COMPARE(pane->path(), p("README.md"));
        key(Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(!pane->isVisible());
        tab()->selectPaths({p("photo.png")}); // chosen while the pane is closed
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(pane->isVisible());
        QTest::qWait(200);
        QCOMPARE(pane->path(), p("photo.png"));
    }

    void terminalKeysSelectionAndScrollback()
    {
        // The terminal's own keys: editing the line, ⌃C, exit and restart with Return, selecting and
        // copying with the mouse, ⌘V, ⌘K, scrolling back, and window titles / folders the shell reports.
        useTestShell();
        TerminalWidget term;
        term.resize(700, 300);
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

    void previewShowsEveryKindOrSaysWhy()
    {
        // Empty files, broken pictures and PDFs say so; a PDF, an animated GIF and a folder are shown;
        // a long text is cut with a note; a zoomed picture can be dragged around.
        QVERIFY(makeFixture(QStringLiteral("kinds")));
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

    void tabsAndWindows()
    {
        auto *tabs = m_win->findChild<QTabWidget *>();
        tab()->focusView();
        key(Qt::Key_T, Qt::ControlModifier);
        QTRY_COMPARE(tabs->count(), 2);
        tab()->navigate(p("Beta"));
        shot("7-tabs");
        key(Qt::Key_W, Qt::ControlModifier);
        QTRY_COMPARE(tabs->count(), 1);
        QCOMPARE(tab()->path(), m_tmp.path());

        const int before = QApplication::topLevelWidgets().size();
        int windows = 0;
        key(Qt::Key_N, Qt::ControlModifier);
        for (QWidget *w : QApplication::topLevelWidgets())
            windows += qobject_cast<MainWindow *>(w) && w->isVisible();
        QCOMPARE(windows, 2);
        QVERIFY(QApplication::topLevelWidgets().size() > before);
    }
};

QTEST_MAIN(Smoke)
#include "smoke.moc"
