// End-to-end tests: one real MainWindow on a scratch folder, driven with synthetic key presses
// the way a user would. Headless like every suite (tests/Headless.h: Qt's offscreen platform, no
// window on the screen, the keyboard never taken from the user). What needs no main window lives in
// unit_widgets (single widgets) and unit_core / unit_fileops / unit_update (logic).
// Screenshots go to $OUT (if set):
//
//   OUT=/tmp/shots ./build/gifiles_smoke

#include "Headless.h"

#include "App.h"
#include "BrowserTab.h"
#include "MainWindow.h"
#include "FileProxy.h"
#include "FolderTree.h"
#include "ItemDelegate.h"
#include "OpenWith.h"
#include "PathBar.h"
#include "RecentFolders.h"
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
            (void)QTest::qWaitForWindowActive(m_win, 500);
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

    void commandDownOpensFolder()
    {
        // Finder's key on every platform (the defaults themselves: unit_core shortcutDefaults).
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
        key(Qt::Key_Left, Qt::ControlModifier); // ⌘← back, ⌘→ forward
        QTRY_COMPARE(tab()->path(), p("Alpha"));
        key(Qt::Key_Right, Qt::ControlModifier);
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

    void revealActionIsInTheMenus()
    {
        // The command lines themselves: unit_core revealCommand.
        bool found = false;
        for (QAction *a : m_win->actions())
            found = found || a->text() == util::revealActionText();
        QVERIFY(found);
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

    void reboundKeyWorksInTheWindow()
    {
        // The capture window and the rebinding rules: unit_widgets shortcutsCanBeRebound.
        Shortcuts *sc = Shortcuts::instance();
        const auto restore = qScopeGuard([sc] { sc->reset(QStringLiteral("열기")); });
        sc->assign(QStringLiteral("열기"), QKeySequence(QStringLiteral("Ctrl+J")));
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
        tab()->focusView();
        tab()->selectPaths({p("Alpha")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        key(Qt::Key_Down, Qt::ControlModifier); // ⌘↓ was replaced: nothing opens
        QTest::qWait(200);
        QCOMPARE(tab()->path(), m_tmp.path());
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

    void settingsShortcutOpensTerminalPageFromTerminal()
    {
        tab()->focusView();
        key(Qt::Key_Comma, Qt::ControlModifier); // ⌘ on macOS, Ctrl on Windows/Linux
        QPointer<SettingsDialog> dialog;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *settings = qobject_cast<SettingsDialog *>(w); settings && settings->isVisible())
                dialog = settings;
        QVERIFY(dialog);
        const auto closeDialog = qScopeGuard([&] { if (dialog) dialog->close(); });
        auto *nav = dialog->findChild<QListWidget *>(QStringLiteral("settingsNav"));
        QVERIFY(nav && nav->currentItem());
        QCOMPARE(nav->currentItem()->text(), QStringLiteral("일반"));
        dialog->hide(); // Reusing the existing dialog must switch away from its previous page too.
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
        key(Qt::Key_Down, Qt::AltModifier);
        auto *terminal = qobject_cast<TerminalWidget *>(focus());
        QVERIFY(terminal);
        key(Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(dialog->isVisible());
        QCOMPARE(nav->currentItem()->text(), QStringLiteral("터미널"));
        shot(QStringLiteral("settings-from-terminal"), dialog);
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
            (void)QTest::qWaitForWindowActive(m_win);
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
        QTRY_VERIFY2(terminal->screenText().contains(QStringLiteral("keep_this_line")),
                     qPrintable(QStringLiteral("size=%1x%2 cell=%3x%4 font=%5 screen=[%6]")
                                    .arg(terminal->width()).arg(terminal->height())
                                    .arg(static_cast<QWidget *>(terminal.data())->inputMethodQuery(Qt::ImCursorRectangle).toRect().width())
                                    .arg(static_cast<QWidget *>(terminal.data())->inputMethodQuery(Qt::ImCursorRectangle).toRect().height())
                                    .arg(static_cast<QWidget *>(terminal.data())->inputMethodQuery(Qt::ImFont).value<QFont>().family(), terminal->screenText())));
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

    // Into a folder and straight back while Qt's file-info thread is still busy: QFileSystemModel asks
    // for the folder again, finds the first request still queued and skips watching it, so the list
    // used to stop following the folder for good (CI: every later makeFixture timed out).
    void folderKeepsFollowingChangesAfterQuickReturn()
    {
        const auto restore = qScopeGuard([this] {
            tab()->navigate(m_tmp.path());
            QDir(p("quick")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("quick")));
        QVERIFY(QDir().mkpath(p("quick/big")));
        QVERIFY(QDir().mkpath(p("quick/home/sub")));
        for (int i = 0; i < 3000; ++i)
            write(QStringLiteral("quick/big/f%1.txt").arg(i), "x");
        tab()->setMode(BrowserTab::List, true);
        tab()->navigate(p("quick/big")); // keeps the file-info thread busy for a while
        tab()->navigate(p("quick/home"));
        tab()->navigate(p("quick/home/sub"));
        tab()->navigate(p("quick/home"));
        QTRY_COMPARE(shownNames(), QStringList{QStringLiteral("sub")});
        QTest::qWait(500);
        write(QStringLiteral("quick/home/new.txt"), "x");
        QTRY_COMPARE_WITH_TIMEOUT(shownNames(), (QStringList{QStringLiteral("sub"), QStringLiteral("new.txt")}), 5000);
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
            QTreeWidgetItem *volume = sb->topLevelItem(sb->topLevelItemCount() - 1)->child(0); // "위치" is the last section
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
        QTreeWidgetItem *volume = sb->topLevelItem(sb->topLevelItemCount() - 1)->child(0); // "위치" is the last section
        QVERIFY(volume);
        const QString root = QDir::fromNativeSeparators(volume->toolTip(0));
        QVERIFY(QFileInfo(root).isDir());
        sb->setFocus();
        QTRY_VERIFY(sb->hasFocus());
        for (int i = 0; i < 80 && sb->currentItem() != volume; ++i) // past any recent folders
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

    // Tabs between windows: a tab moves into another window with its history; pulled out it becomes a window
    // of its own; 모든 윈도우 합치기 gathers every tab, and a window left without tabs closes. The drags
    // themselves (tabBarEvent, the system window move) can't be driven offscreen; their drop target can.
    void tabsMoveBetweenWindows()
    {
        auto *tabs1 = m_win->findChild<QTabWidget *>();
        const int before = tabs1->count();
        QPointer<MainWindow> w2 = App::instance()->newWindow({p("Alpha"), p("Beta")}, m_win);
        QPointer<MainWindow> w3;
        const auto restore = qScopeGuard([&] {
            for (const QPointer<MainWindow> &w : {w2, w3})
                if (w)
                    w->close();
            while (tabs1->count() > before)
                emit tabs1->tabCloseRequested(tabs1->count() - 1);
            m_win->activateWindow();
            (void)QTest::qWaitForWindowActive(m_win);
        });
        QVERIFY(QTest::qWaitForWindowExposed(w2));
        auto *tabs2 = w2->findChild<QTabWidget *>();
        // The drop target is the other window whose toolbar + tab bar holds the point.
        QCOMPARE(m_win->tabDropTarget(w2->tabDropZone().center()), w2.data());
        QVERIFY(!m_win->tabDropTarget(w2->tabDropZone().bottomLeft() + QPoint(0, 40)));
        QVERIFY(!w2->tabDropTarget(w2->tabDropZone().center())); // never its own window

        auto *beta = static_cast<BrowserTab *>(tabs2->widget(1));
        beta->navigate(p("Alpha"));
        QTRY_COMPARE(beta->path(), p("Alpha"));
        w2->moveTabTo(1, m_win);
        QCOMPARE(tabs2->count(), 1);
        QCOMPARE(tabs1->count(), before + 1);
        QCOMPARE(m_win->tab(), beta);
        QVERIFY(beta->canGoBack()); // its history came along
        QCOMPARE(tabs1->tabText(tabs1->indexOf(beta)), util::displayName(p("Alpha")));

        w3 = m_win->detachTab(tabs1->indexOf(beta), m_win->frameGeometry().topRight() + QPoint(300, 200));
        QVERIFY(w3);
        QCOMPARE(w3->tab(), beta);
        QCOMPARE(w3->findChild<QTabWidget *>()->count(), 1);
        QCOMPARE(tabs1->count(), before);
        QVERIFY(!w2->detachTab(0, QPoint())); // a window's only tab stays put

        // Pulled off the tab bar with the mouse and let go outside every window: a window of its own.
        w2->addTab(p("Beta"), false, true);
        QTabBar *bar = tabs2->tabBar();
        QTRY_VERIFY(bar->isVisible());
        QPointer<BrowserTab> pulled = static_cast<BrowserTab *>(tabs2->widget(1));
        const QPoint from = bar->tabRect(1).center();
        QTest::mousePress(bar, Qt::LeftButton, {}, from);
        QTest::mouseMove(bar, from + QPoint(0, 3 * bar->height()));
        const QPoint away = w2->frameGeometry().bottomRight() + QPoint(200, 200);
        QTest::mouseMove(bar, bar->mapFromGlobal(away));
        QTest::mouseRelease(bar, Qt::LeftButton, {}, bar->mapFromGlobal(away));
        QCOMPARE(tabs2->count(), 1);
        QVERIFY(pulled && pulled->window() != w2.data() && pulled->window() != m_win);
        QPointer<MainWindow> w4 = qobject_cast<MainWindow *>(pulled->window());
        QVERIFY(w4);
        const auto closeW4 = qScopeGuard([w4] {
            if (w4)
                w4->close();
        });

        m_win->mergeAllWindows();
        QCOMPARE(tabs1->count(), before + 3);
        QTRY_VERIFY(!w4 || !w4->isVisible());
        QTRY_VERIFY(!w2 || !w2->isVisible());
        QTRY_VERIFY(!w3 || !w3->isVisible());
        QVERIFY(tabs1->indexOf(beta) >= 0);
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

    // ` opens the NCD-style folder tree over the file views: typing jumps to the best match,
    // Tab / ⇧Tab to the next ones, arrows walk the tree, Return goes there; Esc or ` closes.
    void folderTreeJumpsToTypedFolder()
    {
        for (const char *d : {"tree/one/target", "tree/two/target2", "tree/two/other"})
            QVERIFY(QDir().mkpath(p(QString::fromUtf8(d))));
        Settings::instance()->setValue(Settings::FolderTreeRoots, QStringList{p(QStringLiteral("tree"))});
        tab()->navigate(p(QStringLiteral("tree/two")));
        QTRY_COMPARE(tab()->path(), p(QStringLiteral("tree/two")));
        tab()->focusView();
        auto norm = [](const QString &path) { return QDir::cleanPath(QDir::fromNativeSeparators(path)); };

        QTest::keyClick(focus(), '`');
        auto *panel = m_win->findChild<FolderTreePanel *>();
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        QCOMPARE(QApplication::focusWidget(), panel->queryEdit());
        // Over the whole window (toolbar, sidebar and terminal included), following its size.
        QCOMPARE(panel->geometry(), m_win->rect());
        const QSize before = m_win->size();
        m_win->resize(before + QSize(30, 20));
        QTRY_COMPARE(panel->geometry(), m_win->rect());
        m_win->resize(before);
        QTRY_VERIFY(FolderTree::instance()->index() && !FolderTree::instance()->isScanning());
        QTRY_COMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/two")))); // opens where the browser is
        shot("folder-tree", panel);

        QTest::keyClicks(panel->queryEdit(), QStringLiteral("target"));
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one/target")))); // the exact name first
        QTest::keyClick(panel->queryEdit(), Qt::Key_Tab);
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/two/target2"))));
        QTest::keyClick(panel->queryEdit(), Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one/target"))));
        // Across folders in path order: one + tar(get).
        panel->queryEdit()->clear();
        QTest::keyClicks(panel->queryEdit(), QStringLiteral("onetar"));
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one/target"))));
        // Aa: match case (⌥C), remembered in config.toml.
        panel->queryEdit()->clear();
        QTest::keyClick(panel->queryEdit(), Qt::Key_C, Qt::AltModifier);
        QVERIFY(panel->caseButton()->isChecked());
        QVERIFY(Settings::instance()->flag(Settings::FolderTreeCase));
        QVERIFY(panel->queryEdit()->text().isEmpty());
        QTest::keyClicks(panel->queryEdit(), QStringLiteral("TARGET"));
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one/target")))); // no match: stays
        panel->caseButton()->click();
        QVERIFY(!Settings::instance()->flag(Settings::FolderTreeCase));
        QTest::keyClick(panel->queryEdit(), Qt::Key_Tab);
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/two/target2"))));
        QTest::keyClick(panel->queryEdit(), Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one/target"))));
        // The panel's keys are listed at the bottom (fixed, not in Settings → 단축키).
        auto *keys = panel->findChild<QLabel *>(QStringLiteral("folderTreeKeys"));
        QVERIFY(keys && keys->isVisible());
        QVERIFY(keys->text().contains(QStringLiteral("<b>Tab</b>")));
        QVERIFY(keys->text().contains(QStringLiteral("Enter")));
        for (const Shortcuts::Entry &e : Shortcuts::entries())
            QVERIFY2(!e.id.startsWith(QStringLiteral("폴더 트리 ")), qPrintable(e.id));
        QTest::keyClick(panel->queryEdit(), Qt::Key_Up); // the arrows walk the tree
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one"))));
        QCOMPARE(QApplication::focusWidget(), panel->queryEdit());
        QTest::keyClick(panel->queryEdit(), Qt::Key_Right); // NCD: → into the first subfolder, ← to the parent
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one/target"))));
        QTest::keyClick(panel->queryEdit(), Qt::Key_Left);
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one"))));
        // Always fully unfolded: every folder is a row.
        QCOMPARE(panel->view()->model()->rowCount(), 6);
        QTest::keyClick(panel->queryEdit(), Qt::Key_Return); // Return (or ⌘↓) goes in
        QVERIFY(!panel->isVisible());
        QTRY_COMPARE(norm(tab()->path()), norm(p(QStringLiteral("tree/one"))));
        QTRY_VERIFY(tab()->isAncestorOf(QApplication::focusWidget()));

        // Esc closes and stays; so does ` (also typed as ₩ by the Korean input source).
        QTest::keyClick(focus(), '`');
        QTRY_VERIFY(panel->isVisible());
        QTRY_COMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one"))));
        QTest::keyClicks(panel->queryEdit(), QStringLiteral("other"));
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/two/other"))));
        QTest::keyClick(panel->queryEdit(), Qt::Key_Escape);
        QVERIFY(!panel->isVisible());
        QCOMPARE(norm(tab()->path()), norm(p(QStringLiteral("tree/one"))));
        QTest::keyClick(focus(), '`');
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(panel->queryEdit()->text().isEmpty());
        QTest::keyClick(panel->queryEdit(), '`');
        QVERIFY(!panel->isVisible());
        QTest::keyClick(focus(), '`');
        QTRY_VERIFY(panel->isVisible());
        panel->queryEdit()->insert(QString(QChar(0x20A9))); // what an input method commits
        QVERIFY(!panel->isVisible());
        QVERIFY(!panel->queryEdit()->text().contains(QChar(0x20A9)));
        QTest::keyClick(focus(), '`'); // the same key then reaches the file view: stays closed
        QVERIFY(!panel->isVisible());
        QTest::qWait(350);

        // From the sidebar too.
        if (auto *sidebar = m_win->findChild<Sidebar *>(); sidebar && sidebar->isVisible()) {
            sidebar->setFocus();
            QTest::keyClick(sidebar, '`');
            QTRY_VERIFY(panel->isVisible());
            QTest::keyClick(panel->queryEdit(), Qt::Key_Escape);
            QVERIFY(!panel->isVisible());
        }

        // ⌘R reads the tree again while it is open.
        QTRY_VERIFY(tab()->isAncestorOf(QApplication::focusWidget()));
        QVERIFY(QDir().mkpath(p(QStringLiteral("tree/two/later"))));
        QTest::keyClick(focus(), '`');
        QTRY_VERIFY(panel->isVisible());
        QTRY_VERIFY(!FolderTree::instance()->isScanning());
        QTest::keyClick(panel->queryEdit(), Qt::Key_R, Qt::ControlModifier);
        QVERIFY(FolderTree::instance()->isScanning());
        QVERIFY(panel->busyLabel()->isVisible()); // "인덱싱 중…" in the middle
        if (const QString out = qEnvironmentVariable("OUT"); !out.isEmpty()) // now: the scan is short
            panel->grab().save(QDir(out).filePath(QStringLiteral("folder-tree-indexing.png")));
        QTRY_VERIFY(!FolderTree::instance()->isScanning());
        QTest::keyClicks(panel->queryEdit(), QStringLiteral("later"));
        QTRY_COMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/two/later"))));
        QTest::keyClick(panel->queryEdit(), Qt::Key_Escape);

#ifdef Q_OS_MACOS
        // FSEvents keeps the tree up to date while it is open: a folder made outside the app shows up
        // without ⌘R, and no full scan runs for it.
        QTRY_VERIFY(tab()->isAncestorOf(QApplication::focusWidget()));
        QTest::keyClick(focus(), '`');
        QTRY_VERIFY(panel->isVisible());
        QTRY_VERIFY(FolderTree::instance()->isWatching());
        QVERIFY(QDir().mkpath(p(QStringLiteral("tree/two/live/inside"))));
        QTRY_VERIFY_WITH_TIMEOUT(FolderTree::instance()->index()->find(p(QStringLiteral("tree/two/live/inside"))) >= 0, 10000);
        QVERIFY(!FolderTree::instance()->isScanning());
        QTest::keyClicks(panel->queryEdit(), QStringLiteral("inside"));
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/two/live/inside"))));
        QVERIFY(QDir(p(QStringLiteral("tree/two/live"))).removeRecursively());
        QTRY_VERIFY_WITH_TIMEOUT(FolderTree::instance()->index()->find(p(QStringLiteral("tree/two/live"))) < 0, 10000);
        QTest::keyClick(panel->queryEdit(), Qt::Key_Escape);
#endif

        // A folder made in the app shows up the next time.
        QTRY_VERIFY(tab()->isAncestorOf(QApplication::focusWidget()));
        QVERIFY(QDir().mkpath(p(QStringLiteral("tree/one/fresh"))));
        FolderTree::instance()->markStale();
        QTest::keyClick(focus(), '`');
        QTRY_VERIFY(panel->isVisible());
        QTRY_VERIFY(!FolderTree::instance()->isScanning());
        QTest::keyClicks(panel->queryEdit(), QStringLiteral("fresh"));
        QTRY_COMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree/one/fresh"))));
        QTest::keyClick(panel->queryEdit(), Qt::Key_Escape);

        // ⌘D: the drive list, modal inside the panel; a drive chosen there is the tree until the app quits.
        QVERIFY(QDir().mkpath(p(QStringLiteral("tree2/far/target"))));
        QTRY_VERIFY(tab()->isAncestorOf(QApplication::focusWidget()));
        QTest::keyClick(focus(), '`');
        QTRY_VERIFY(panel->isVisible());
        QTRY_VERIFY(!FolderTree::instance()->isScanning());
        QVERIFY(!panel->busyLabel()->isVisible());
        QVERIFY(keys->text().contains(QKeySequence(QStringLiteral("Ctrl+D")).toString(QKeySequence::NativeText)));
        QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_D, Qt::ControlModifier);
        QApplication::sendEvent(panel->queryEdit(), &over);
        QVERIFY(over.isAccepted()); // not the menu's 복제
        QTest::keyClick(panel->queryEdit(), Qt::Key_D, Qt::ControlModifier);
        QVERIFY(panel->drivesBox()->isVisible());
        QVERIFY(panel->driveList()->count() >= 2); // the set folders, then the drives
        shot("folder-tree-drives", panel);
        QCOMPARE(panel->driveList()->item(0)->data(Qt::UserRole).toString(), QString());
        QCOMPARE(panel->driveList()->currentRow(), 0); // the tree shown now
        QTest::keyClick(panel->queryEdit(), Qt::Key_Down);
        QCOMPARE(panel->driveList()->currentRow(), 1);
        QTest::keyClicks(panel->queryEdit(), QStringLiteral("x")); // typing goes to the list, not the query
        QVERIFY(panel->queryEdit()->text().isEmpty());
        QTest::keyClick(panel->queryEdit(), Qt::Key_Escape); // closes the list only
        QVERIFY(!panel->drivesBox()->isVisible());
        QVERIFY(panel->isVisible());
        QTest::keyClick(panel->queryEdit(), Qt::Key_D, Qt::ControlModifier);
        QTest::keyClick(panel->queryEdit(), Qt::Key_Return); // the set folders again: nothing changes
        QVERIFY(!panel->drivesBox()->isVisible());
        QCOMPARE(FolderTree::instance()->drive(), QString());
        // Another drive (a folder here, so the test never scans a real disk): scanned, then its tree.
        FolderTree::instance()->setDrive(p(QStringLiteral("tree2")));
        QTRY_VERIFY(FolderTree::instance()->index() && !FolderTree::instance()->isScanning());
        QTRY_COMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree2"))));
        QCOMPARE(panel->view()->model()->rowCount(), 3);
        QTest::keyClicks(panel->queryEdit(), QStringLiteral("target"));
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("tree2/far/target"))));
        QTest::keyClick(panel->queryEdit(), Qt::Key_Escape);
        FolderTree::instance()->setDrive(QString());
        QDir(p(QStringLiteral("tree2"))).removeRecursively();

        Settings::instance()->remove(Settings::FolderTreeRoots);
        tab()->navigate(m_tmp.path());
        QTRY_COMPARE(tab()->path(), m_tmp.path());
        QDir(p(QStringLiteral("tree"))).removeRecursively();
    }

    // The sidebar's "최근 폴더": browsing a folder doesn't put it there, working in it does — a new
    // folder made, a command run in the terminal there (cd and ls are only looking).
    void recentFoldersFollowWorkNotBrowsing()
    {
        useTestShell();
        auto *sb = m_win->findChild<Sidebar *>();
        QVERIFY(sb);
        RecentFolders::instance()->clear();
        const auto restore = qScopeGuard([this] {
            RecentFolders::instance()->clear();
            tab()->navigate(m_tmp.path());
            QDir(p("recent")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("recent")));
        for (const char *d : {"recent/looked", "recent/worked", "recent/typed"})
            QVERIFY(QDir().mkpath(p(QString::fromLatin1(d))));
        auto shown = [sb] {
            QStringList out;
            for (int i = 0; i < sb->topLevelItemCount(); ++i)
                if (sb->topLevelItem(i)->text(0) == QStringLiteral("최근 폴더"))
                    for (int j = 0; j < sb->topLevelItem(i)->childCount(); ++j)
                        out << QDir::fromNativeSeparators(sb->topLevelItem(i)->child(j)->toolTip(0));
            return out;
        };
        tab()->navigate(p("recent/looked"));
        QTRY_COMPARE(tab()->path(), p("recent/looked"));
        tab()->navigate(p("recent/worked"));
        QTRY_COMPARE(tab()->path(), p("recent/worked"));
        tab()->focusView();
        key(Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier); // 새로운 폴더
        QTRY_VERIFY(QDir(p("recent/worked/무제 폴더")).exists());
        QTRY_VERIFY(qobject_cast<QLineEdit *>(focus()));
        key(Qt::Key_Escape);
        QTRY_COMPARE(shown(), QStringList{p("recent/worked")});

        // The terminal: cd and ls don't count, a command that does something does.
        tab()->navigate(p("recent/typed"));
        key(Qt::Key_QuoteLeft, kTerminalMods);
        TerminalWidget *term = nullptr;
        QTRY_VERIFY((term = m_win->findChild<TerminalWidget *>()) && term->isVisible());
        QTRY_VERIFY2_WITH_TIMEOUT(term->isReady(), qPrintable(term->screenText()), 15000);
        QTRY_COMPARE(QFileInfo(term->shellCwd()).canonicalFilePath(), QFileInfo(p("recent/typed")).canonicalFilePath());
        term->setFocus();
#ifdef Q_OS_WIN
        QTest::keyClicks(term, "Get-ChildItem");
#else
        QTest::keyClicks(term, "ls");
#endif
        QTest::keyClick(term, Qt::Key_Return);
        QTest::qWait(500);
        QCOMPARE(shown(), QStringList{p("recent/worked")});
#ifdef Q_OS_WIN
        QTest::keyClicks(term, "Set-Content -Path made.txt -Value x");
#else
        QTest::keyClicks(term, "touch made.txt");
#endif
        QTest::keyClick(term, Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(p("recent/typed/made.txt")));
        auto canon = [](const QStringList &l) {
            QStringList out;
            for (const QString &x : l)
                out << QFileInfo(x).canonicalFilePath();
            return out;
        };
        QTRY_COMPARE(canon(shown()), canon({p("recent/typed"), p("recent/worked")}));
        QVERIFY(!shown().contains(p("recent/looked")));
        key(Qt::Key_QuoteLeft, kTerminalMods); // fold again

        // A click on one opens it.
        tab()->navigate(m_tmp.path());
        QTreeWidgetItem *row = nullptr;
        for (int i = 0; i < sb->topLevelItemCount(); ++i)
            if (sb->topLevelItem(i)->text(0) == QStringLiteral("최근 폴더"))
                row = sb->topLevelItem(i)->child(1);
        QVERIFY(row);
        QTest::mouseClick(sb->viewport(), Qt::LeftButton, {}, sb->visualItemRect(row).center());
        QTRY_COMPARE(tab()->path(), p("recent/worked"));
    }

    // The folder tree by mouse: a double click goes into the folder, a click in the drive list picks
    // the drive; and a Korean name is found while it is still being composed (no Enter needed).
    void folderTreeMouseAndComposition()
    {
        for (const char *d : {"mtree/가나다/깊은곳", "mtree/other", "mtree2/x"})
            QVERIFY(QDir().mkpath(p(QString::fromUtf8(d))));
        Settings::instance()->setValue(Settings::FolderTreeRoots, QStringList{p(QStringLiteral("mtree"))});
        const auto restore = qScopeGuard([this] {
            FolderTree::instance()->setDrive(QString());
            Settings::instance()->remove(Settings::FolderTreeRoots);
            tab()->navigate(m_tmp.path());
            QDir(p(QStringLiteral("mtree"))).removeRecursively();
            QDir(p(QStringLiteral("mtree2"))).removeRecursively();
        });
        auto norm = [](const QString &path) { return QDir::cleanPath(QDir::fromNativeSeparators(path)).normalized(QString::NormalizationForm_C); };
        tab()->navigate(p(QStringLiteral("mtree")));
        QTRY_COMPARE(tab()->path(), p(QStringLiteral("mtree")));
        tab()->focusView();
        QTest::keyClick(focus(), '`');
        auto *panel = m_win->findChild<FolderTreePanel *>();
        QTRY_VERIFY(panel && panel->isVisible());
        QTRY_VERIFY(FolderTree::instance()->index() && !FolderTree::instance()->isScanning());
        QTRY_COMPARE(panel->view()->model()->rowCount(), 4);

        // Composing 깊 (not committed yet) already jumps there.
        QInputMethodEvent composing(QStringLiteral("깊"), {});
        QApplication::sendEvent(panel->queryEdit(), &composing);
        QCOMPARE(norm(panel->currentPath()), norm(p(QStringLiteral("mtree/가나다/깊은곳"))));
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("깊"));
        QApplication::sendEvent(panel->queryEdit(), &commit);
        QCOMPARE(panel->queryEdit()->text(), QStringLiteral("깊"));

        // A double click on a row goes there.
        QListView *view = panel->view();
        QModelIndex other;
        for (int r = 0; r < view->model()->rowCount(); ++r)
            if (view->model()->index(r, 0).data().toString() == QStringLiteral("other"))
                other = view->model()->index(r, 0);
        QVERIFY(other.isValid());
        view->scrollTo(other);
        const QPoint at = view->visualRect(other).center();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, at);
        QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, at);
        QTRY_VERIFY(!panel->isVisible());
        QTRY_COMPARE(norm(tab()->path()), norm(p(QStringLiteral("mtree/other"))));

        // ⌘D, then a click on another drive (a folder here) picks it.
        QTRY_VERIFY(tab()->isAncestorOf(QApplication::focusWidget()));
        QTest::keyClick(focus(), '`');
        QTRY_VERIFY(panel->isVisible());
        QTest::keyClick(panel->queryEdit(), Qt::Key_D, Qt::ControlModifier);
        QVERIFY(panel->drivesBox()->isVisible());
        QListWidget *drives = panel->driveList();
        QTest::keyClick(panel->queryEdit(), Qt::Key_End);
        QCOMPARE(drives->currentRow(), drives->count() - 1);
        QTest::keyClick(panel->queryEdit(), Qt::Key_Home);
        QCOMPARE(drives->currentRow(), 0);
        QTest::keyClick(panel->queryEdit(), Qt::Key_Up); // wraps to the last
        QCOMPARE(drives->currentRow(), drives->count() - 1);
        auto *item = new QListWidgetItem(QStringLiteral("mtree2"), drives); // as a mounted drive would be listed
        item->setData(Qt::UserRole, p(QStringLiteral("mtree2")));
        QTest::mouseClick(drives->viewport(), Qt::LeftButton, {}, drives->visualItemRect(item).center());
        QVERIFY(!panel->drivesBox()->isVisible());
        QCOMPARE(norm(FolderTree::instance()->drive()), norm(p(QStringLiteral("mtree2"))));
        QTRY_VERIFY(FolderTree::instance()->index() && !FolderTree::instance()->isScanning());
        QTRY_COMPARE(panel->view()->model()->rowCount(), 2);
        QTest::keyClick(panel->queryEdit(), Qt::Key_Escape);
    }

    // A "선택한 항목들로…" command run quietly (terminal = false) that fails says so, with what it printed;
    // one that can't even start (a shell that doesn't exist) too. Nothing is selected then.
    void quietCommandFailureIsReported()
    {
        QVariantMap failing{{QStringLiteral("label"), QStringLiteral("실패하는 명령")}, {QStringLiteral("terminal"), false},
#ifdef Q_OS_WIN
                            {QStringLiteral("command"), QStringLiteral("Write-Output 'went wrong'; exit 3")}};
#else
                            {QStringLiteral("command"), QStringLiteral("echo went wrong; exit 3")}};
#endif
        tab()->selectPaths({p("README.md")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        QPointer<QMessageBox> box;
        auto shown = [&] {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *b = qobject_cast<QMessageBox *>(w); b && b->isVisible())
                    return QPointer<QMessageBox>(b);
            for (QMessageBox *b : m_win->findChildren<QMessageBox *>())
                if (b->isVisible())
                    return QPointer<QMessageBox>(b);
            return QPointer<QMessageBox>();
        };
        m_win->runSelectionCommand(failing, tab()->selectedPaths());
        QTRY_VERIFY_WITH_TIMEOUT((box = shown()), 20000);
        QVERIFY2(box->text().contains(QStringLiteral("3")), qPrintable(box->text()));
        QVERIFY(box->informativeText().contains(QStringLiteral("went wrong")));
        QCOMPARE(box->detailedText(), TerminalWidget::expandCommand(failing.value(QStringLiteral("command")).toString(), tab()->selectedPaths(), QString(), tab()->path()));
        box->close();
        QTRY_VERIFY(!box);
#ifndef Q_OS_WIN
        Settings::instance()->setValue(Settings::TermShell, QStringLiteral("/no/such/shell"));
        const auto restore = qScopeGuard([] { Settings::instance()->setValue(Settings::TermShell, QString()); });
        m_win->runSelectionCommand(failing, tab()->selectedPaths());
        QTRY_VERIFY_WITH_TIMEOUT((box = shown()), 20000);
        QVERIFY2(box->text().contains(QStringLiteral("/no/such/shell")), qPrintable(box->text()));
        box->close();
        QTRY_VERIFY(!box);
#endif
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
    }

    // Dropping with a modifier: Finder's ⌥ copies and ⌘ moves (Windows/Linux: Ctrl copies, Shift moves).
    void dropModifiersForceCopyOrMove()
    {
        const auto restore = qScopeGuard([this] {
            tab()->navigate(m_tmp.path());
            QDir(p("mods")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("mods")));
        QVERIFY(QDir().mkpath(p("mods/into")));
        write(QStringLiteral("mods/a.txt"), "a");
        tab()->navigate(p("mods"));
        tab()->setMode(BrowserTab::List, true);
        QTRY_COMPARE(tab()->itemCount(), 2);
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        auto *proxy = static_cast<FileProxy *>(tree->model());
        auto dropWith = [&](Qt::Key modKey, Qt::KeyboardModifiers mods) {
            QTest::keyPress(tree->viewport(), modKey, mods); // what the system reports while dragging
            QMimeData md;
            md.setUrls({QUrl::fromLocalFile(p("mods/a.txt"))});
            const QPoint at = tree->visualRect(proxy->indexForPath(p("mods/into"))).center();
            QDragEnterEvent enter(at, Qt::CopyAction | Qt::MoveAction, &md, Qt::LeftButton, mods);
            QApplication::sendEvent(tree->viewport(), &enter);
            QDragMoveEvent move(at, Qt::CopyAction | Qt::MoveAction, &md, Qt::LeftButton, mods);
            QApplication::sendEvent(tree->viewport(), &move);
            QDropEvent drop(QPointF(at), Qt::CopyAction | Qt::MoveAction, &md, Qt::LeftButton, mods);
            QApplication::sendEvent(tree->viewport(), &drop);
            QTest::keyRelease(tree->viewport(), modKey, Qt::NoModifier);
        };
#ifdef Q_OS_MACOS
        dropWith(Qt::Key_Alt, Qt::AltModifier); // ⌥: copy, the original stays
#else
        dropWith(Qt::Key_Control, Qt::ControlModifier);
#endif
        QTRY_VERIFY(QFile::exists(p("mods/into/a.txt")));
        QVERIFY(QFile::exists(p("mods/a.txt")));
        QTRY_VERIFY(App::instance()->canUndo());
        QVERIFY(QFile::remove(p("mods/into/a.txt")));
        QTest::qWait(300);
#ifdef Q_OS_MACOS
        dropWith(Qt::Key_Control, Qt::ControlModifier); // ⌘ (Qt's Control): move
#else
        dropWith(Qt::Key_Shift, Qt::ShiftModifier);
#endif
        QTRY_VERIFY(QFile::exists(p("mods/into/a.txt")));
        QTRY_VERIFY(!QFile::exists(p("mods/a.txt")));
        QTRY_COMPARE(QGuiApplication::queryKeyboardModifiers(), Qt::NoModifier);
    }

    // 중단 in the status bar stops every running job (two copies at once here; it used to stop only the
    // last one started), and nothing is left half-copied.
    void cancelStopsEveryRunningJob()
    {
        const auto restore = qScopeGuard([this] {
            tab()->navigate(m_tmp.path());
            QDir(p("jobs")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("jobs")));
        for (const char *d : {"jobs/big", "jobs/one", "jobs/two"})
            QVERIFY(QDir().mkpath(p(QString::fromLatin1(d))));
        for (int i = 0; i < 4000; ++i)
            write(QStringLiteral("jobs/big/f%1.txt").arg(i), QByteArray(64, 'x'));
        tab()->navigate(p("jobs"));
        tab()->setMode(BrowserTab::List, true);
        QTRY_COMPARE(tab()->itemCount(), 3);
        QToolButton *cancel = nullptr;
        for (QToolButton *b : m_win->findChildren<QToolButton *>())
            if (b->toolTip() == Gifiles::tr("중단"))
                cancel = b;
        QVERIFY(cancel && !cancel->isVisible());
        tab()->selectPaths({p("jobs/big")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        tab()->focusView();
        key(Qt::Key_C, Qt::ControlModifier);
        for (const char *into : {"jobs/one", "jobs/two"}) {
            tab()->navigate(p(QString::fromLatin1(into)));
            QTRY_COMPARE(tab()->path(), p(QString::fromLatin1(into)));
            tab()->focusView();
            key(Qt::Key_V, Qt::ControlModifier);
        }
        QTRY_VERIFY(cancel->isVisible());
        cancel->click();
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 20000); // both jobs are over
        // A folder copy stopped half-way is taken away again: neither copy is left incomplete.
        QTRY_VERIFY(!QDir(p("jobs/one/big")).exists());
        QTRY_VERIFY(!QDir(p("jobs/two/big")).exists());
        QCOMPARE(QDir(p("jobs/big")).entryList(QDir::Files).size(), 4000); // the source is whole
    }

    // List keys and mouse beyond the basics: ⌘⌥→ unfolds a folder with everything inside it (folders
    // not read yet too), ⌘⌥← folds it all again (⌥← / ⌥→ alone move between the panes); ⌘A selects all; a double click enters a folder; a right
    // click selects the item under it (on empty space: nothing); after ⌘⌫ the next item is selected
    // (the one above when the last went).
    void listUnfoldsSelectsAndRightClicks()
    {
        const auto restore = qScopeGuard([this] {
            tab()->navigate(m_tmp.path());
            QDir(p("deep")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("deep")));
        QVERIFY(QDir().mkpath(p("deep/a/b/c/d")));
        write(QStringLiteral("deep/a/b/c/d/leaf.txt"), "x");
        write(QStringLiteral("deep/x.txt"), "x");
        write(QStringLiteral("deep/y.txt"), "y");
        tab()->navigate(p("deep"));
        tab()->setMode(BrowserTab::List, true);
        QTRY_COMPARE(tab()->itemCount(), 3);
        auto *tree = qobject_cast<QTreeView *>(tab()->view());
        auto *proxy = static_cast<FileProxy *>(tree->model());
        auto expanded = [&](const char *rel) {
            const QModelIndex i = proxy->indexForPath(p(QString::fromLatin1(rel)));
            return i.isValid() && tree->isExpanded(i);
        };
        tab()->selectPaths({p("deep/a")});
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("deep/a")});
        tab()->focusView();
        key(Qt::Key_Right, Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY(expanded("deep/a"));
        QTRY_VERIFY2(expanded("deep/a/b") && expanded("deep/a/b/c") && expanded("deep/a/b/c/d"), "⌘⌥→ unfolds every level");
        QTRY_VERIFY(proxy->indexForPath(p("deep/a/b/c/d/leaf.txt")).isValid());
        key(Qt::Key_Left, Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY(!expanded("deep/a"));
        key(Qt::Key_Right); // plain → afterwards: one level only, the inner ones closed too
        QTRY_VERIFY(expanded("deep/a"));
        QVERIFY(!expanded("deep/a/b"));
        key(Qt::Key_Left);
        QTRY_VERIFY(!expanded("deep/a"));

        // ⌘A
        key(Qt::Key_A, Qt::ControlModifier);
        QTRY_COMPARE(tab()->selectedPaths().size(), 3);

        // Right click: on an item outside the selection it selects that one; on empty space, nothing.
        auto rightClick = [&](const QPoint &at) {
            onNextPopup([](QWidget *w) { w->close(); });
            QContextMenuEvent e(QContextMenuEvent::Mouse, at, tree->viewport()->mapToGlobal(at));
            QApplication::sendEvent(tree->viewport(), &e);
            QTest::qWait(100);
        };
        tab()->selectPaths({p("deep/x.txt")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        rightClick(tree->visualRect(proxy->indexForPath(p("deep/y.txt"))).center());
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("deep/y.txt")});
        rightClick(QPoint(20, tree->viewport()->height() - 5));
        QTRY_VERIFY(tab()->selectedPaths().isEmpty());

        // ⌘⌫ on the last item selects the one above. (Offscreen, a closed menu leaves no active window.)
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
        tab()->selectPaths({p("deep/y.txt")});
        QTRY_COMPARE(tab()->selectedPaths().size(), 1);
        tab()->focusView();
        key(Qt::Key_Backspace, Qt::ControlModifier);
        QTRY_VERIFY(!QFile::exists(p("deep/y.txt")));
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{p("deep/x.txt")});
        QTRY_VERIFY(App::instance()->canUndo());
        key(Qt::Key_Z, Qt::ControlModifier); // back from the trash
        QTRY_VERIFY(QFile::exists(p("deep/y.txt")));

        // A double click enters a folder.
        const QRect a = tree->visualRect(proxy->indexForPath(p("deep/a")));
        const QPoint onName(a.left() + tree->iconSize().width() + 8, a.center().y());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, onName); // as the system sends it: a click, then the double click
        QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(onName), tree->viewport()->mapToGlobal(QPointF(onName)), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &dbl);
        QTest::mouseRelease(tree->viewport(), Qt::LeftButton, {}, onName);
        QTRY_COMPARE(tab()->path(), p("deep/a"));
    }

    // Column view: the column the keyboard went into extends its selection (⇧↓, ⌘A) and so does the
    // mouse (⇧-click, ⌘-click) — what is shown selected is what the tab acts on; Return renames there.
    void columnsSelectAndRename()
    {
        const auto restore = qScopeGuard([this] {
            tab()->setMode(BrowserTab::List, true);
            tab()->navigate(m_tmp.path());
            QDir(p("cols")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("cols")));
        QVERIFY(QDir().mkpath(p("cols/inner")));
        for (const char *f : {"one.txt", "two.txt", "three.txt"})
            write(QStringLiteral("cols/inner/") + QLatin1String(f), "x");
        const QString one = p("cols/inner/one.txt"), two = p("cols/inner/two.txt"), three = p("cols/inner/three.txt");
        tab()->navigate(p("cols"));
        tab()->setMode(BrowserTab::Columns, true);
        tab()->selectPaths({p("cols/inner")});
        QTRY_COMPARE(tab()->path(), p("cols/inner"));
        tab()->focusView();
        key(Qt::Key_Right); // into its column, once it is read
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{one});
        QCoreApplication::processEvents(); // as between two real key presses
        key(Qt::Key_Down, Qt::ShiftModifier);
        QTRY_COMPARE(tab()->selectedPaths(), (QStringList{one, three})); // by name: one, three, two
        key(Qt::Key_A, Qt::ControlModifier);
        QTRY_COMPARE(tab()->selectedPaths(), (QStringList{one, three, two}));

        auto *col = qobject_cast<QListView *>(focus());
        QVERIFY(col);
        auto *proxy = static_cast<FileProxy *>(col->model());
        auto at = [&](const QString &path) { return col->visualRect(proxy->indexForPath(path)).center(); };
        QTest::mouseClick(col->viewport(), Qt::LeftButton, {}, at(one));
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{one});
        QTest::mouseClick(col->viewport(), Qt::LeftButton, Qt::ShiftModifier, at(two));
        QTRY_COMPARE(tab()->selectedPaths(), (QStringList{one, three, two}));
        QTest::mouseClick(col->viewport(), Qt::LeftButton, Qt::ControlModifier, at(three)); // ⌘-click: out again
        QTRY_COMPARE(tab()->selectedPaths(), (QStringList{one, two}));

        tab()->selectPaths({one});
        QTRY_COMPARE(tab()->selectedPaths(), QStringList{one});
        tab()->focusView();
        key(Qt::Key_Return);
        QTRY_VERIFY(qobject_cast<QLineEdit *>(focus()));
        auto *le = qobject_cast<QLineEdit *>(focus());
        QTRY_COMPARE(le->selectedText(), QStringLiteral("one"));
        type("uno");
        key(Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(p("cols/inner/uno.txt")));
        QVERIFY(!QFile::exists(one));
    }

    // Gallery tiles: a long name takes two lines (the second shortened in the middle); painting a folder
    // of such names and selected tiles in item colors works in both themes.
    void galleryLongNamesTakeTwoLines()
    {
        const QVariant theme = Settings::instance()->value(Settings::ThemeMode);
        const auto restore = qScopeGuard([this, theme] {
            Settings::instance()->setValue(Settings::ThemeMode, theme);
            Settings::instance()->remove(Settings::FileColorSelection);
            tab()->setMode(BrowserTab::List, true);
            tab()->navigate(m_tmp.path());
            QDir(p("tiles")).removeRecursively();
        });
        QVERIFY(makeFixture(QStringLiteral("tiles")));
        write(QStringLiteral("tiles/a rather long file name that cannot fit on one line of a tile.txt"), "x");
        write(QStringLiteral("tiles/한글로 된 아주 긴 파일 이름이 두 줄로 나뉘어 보이는지 확인하는 문서.md"), "x");
        write(QStringLiteral("tiles/short.zip"), "x");
        tab()->navigate(p("tiles"));
        tab()->setMode(BrowserTab::Gallery, true);
        QTRY_COMPARE(tab()->itemCount(), 3);
        Settings::instance()->setValue(Settings::FileColorSelection, true);
        for (const char *mode : {"light", "dark"}) {
            Settings::instance()->setValue(Settings::ThemeMode, QString::fromLatin1(mode));
            key(Qt::Key_A, Qt::ControlModifier);
            QTRY_COMPARE(tab()->selectedPaths().size(), 3);
            const QImage img = tab()->view()->viewport()->grab().toImage();
            QVERIFY(!img.isNull());
            shot(QStringLiteral("gallery-long-names-%1").arg(QLatin1String(mode)));
        }
    }

    // Terminal tabs dragged into another order keep each tab with its shell; a click on a tab of the
    // folded terminal unfolds it on that tab. The status bar's slider sizes the gallery's icons;
    // 업데이트 확인… says why a build by hand doesn't update.
    void terminalTabOrderIconSliderAndUpdateCheck()
    {
        useTestShell();
        auto *bar = m_win->findChild<QTabBar *>(QStringLiteral("termTabs"));
        auto *stack = m_win->findChild<QStackedWidget *>(QStringLiteral("termStack"));
        QVERIFY(bar && stack);
        tab()->navigate(p("Alpha"));
        key(Qt::Key_QuoteLeft, kTerminalMods);
        QTRY_COMPARE(bar->count(), 1);
        auto *first = qobject_cast<TerminalWidget *>(stack->widget(0));
        QTRY_VERIFY(first && first->isReady());
        first->setFocus();
        key(Qt::Key_T, Qt::ControlModifier); // a second tab
        QTRY_COMPARE(bar->count(), 2);
        auto *second = qobject_cast<TerminalWidget *>(stack->widget(1));
        QVERIFY(second && second != first);
        bar->moveTab(1, 0); // as a drag of the tab does
        QCOMPARE(stack->widget(0), static_cast<QWidget *>(second));
        QCOMPARE(stack->widget(1), static_cast<QWidget *>(first));
        QCOMPARE(stack->currentWidget(), stack->widget(bar->currentIndex()));
        key(Qt::Key_QuoteLeft, kTerminalMods); // fold
        QTRY_VERIFY(!first->isVisible() && !second->isVisible());
        auto *shown = qobject_cast<TerminalWidget *>(stack->widget(bar->currentIndex()));
        QTest::mouseClick(bar, Qt::LeftButton, {}, bar->tabRect(bar->currentIndex()).center());
        QTRY_VERIFY(shown->isVisible());
        QTRY_VERIFY(shown->hasFocus());
        key(Qt::Key_QuoteLeft, kTerminalMods);
        QTRY_VERIFY(!shown->isVisible());
        tab()->navigate(m_tmp.path());

        // The icon size slider (gallery): this folder's size, and the default for new tabs.
        QSlider *slider = nullptr;
        for (QSlider *sl : m_win->findChildren<QSlider *>())
            if (sl->toolTip() == Gifiles::tr("아이콘 크기"))
                slider = sl;
        QVERIFY(slider);
        const QVariant before = Settings::instance()->value(Settings::IconSize);
        const auto restore = qScopeGuard([before] { Settings::instance()->setValue(Settings::IconSize, before); });
        tab()->setMode(BrowserTab::Gallery, true);
        slider->setValue(150);
        QCOMPARE(Settings::instance()->value(Settings::IconSize).toInt(), 150);
        QCOMPARE(tab()->view()->iconSize().width(), 150);
        tab()->setMode(BrowserTab::List, true);

        // 업데이트 확인…: off in a build by hand, and it says why.
        QString said;
        onNextPopup([&](QWidget *w) {
            if (auto *box = qobject_cast<QMessageBox *>(w)) {
                said = box->text();
                box->accept();
            }
        });
        m_win->findChild<QAction *>(QStringLiteral("업데이트 확인…"))->trigger();
        QTRY_VERIFY(!said.isEmpty());
        QVERIFY2(said.contains(QStringLiteral("Gifiles")) && said.count(QLatin1Char('\n')) >= 2, qPrintable(said));
        m_win->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
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
