#include "App.h"
#include "Util.h"
#include "Log.h"
#include "MainWindow.h"
#include "Permissions.h"
#include "Preview.h"
#include "Settings.h"
#include "Updater.h"
#include "Sidebar.h"
#include "Theme.h"
#ifdef Q_OS_MACOS
#include "MacWindow.h"
#endif
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

#include <cstdio>

#include <QApplication>
#include <QFile>
#include <QLibraryInfo>
#include <QTranslator>
#include <QDir>
#include <QFileInfo>
#include <QAbstractItemView>
#include <QElapsedTimer>
#include <QIcon>
#include <QKeyEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

namespace {

void setNames()
{
    QCoreApplication::setOrganizationName(QStringLiteral("zidell"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("zidell.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("Gifiles"));
    QCoreApplication::setApplicationVersion(QStringLiteral(GIFILES_VERSION));
}

// Text for --help and friends. A Windows GUI program has no console of its own: write to the
// handle it was given (a pipe or file when redirected), else attach to the parent's console.
void print(const QString &text, bool toStderr = false)
{
    const QByteArray utf8 = text.toUtf8();
#ifdef Q_OS_WIN
    const DWORD which = toStderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            h = GetStdHandle(which);
    }
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (GetFileType(h) == FILE_TYPE_CHAR) {
            const std::wstring w = text.toStdWString();
            WriteConsoleW(h, w.c_str(), DWORD(w.size()), &written, nullptr);
        } else {
            WriteFile(h, utf8.constData(), DWORD(utf8.size()), &written, nullptr);
        }
    }
#else
    FILE *out = toStderr ? stderr : stdout;
    fwrite(utf8.constData(), 1, size_t(utf8.size()), out);
    fflush(out);
#endif
}

const char *kHelp = QT_TRANSLATE_NOOP("Gifiles",
    "Gifiles - 터미널이 붙은 파일 관리자\n"
    "\n"
    "사용법: Gifiles [폴더나 파일 ...]\n"
    "       Gifiles --config-path | --print-config | --print-default-config | --check-config [파일] | --version | --help\n"
    "\n"
    "  --config-path            설정 파일(config.toml)의 절대 경로를 출력합니다 (아직 없으면 만들 위치).\n"
    "  --print-config           지금 시작하면 쓸 설정을 출력합니다: 파일의 값, 나머지는 기본값.\n"
    "                           잘못된 값은 기본값으로 나오고 문제는 오류 출력으로 알립니다.\n"
    "  --print-default-config   모든 항목이 기본값인 설정 파일을 설명 주석과 함께 출력합니다.\n"
    "  --check-config [파일]    설정 파일을 검사해 문제를 출력합니다. 문제가 있으면 종료 코드 1.\n"
    "  --version                버전을 출력합니다.\n"
    "  --help                   이 도움말.\n"
    "\n"
    "설정은 config.toml 하나에 있습니다 (macOS: ~/Library/Application Support/Gifiles,\n"
    "Windows: %APPDATA%\\Gifiles, Linux: ${XDG_CONFIG_HOME:-~/.config}/gifiles). 각 항목 위에 뜻과\n"
    "허용값이 적혀 있고, 앱이 실행 중이면 저장하는 즉시 다시 읽습니다. 환경 변수 GIFILES_CONFIG_DIR로\n"
    "폴더를 바꿀 수 있습니다. 자세한 안내: 설치된 readme.txt (macOS: Gifiles.app/Contents/Resources,\n"
    "Windows: Gifiles.exe 옆, Linux AppImage: --appimage-extract 후 squashfs-root/usr/share/doc/gifiles).\n");

// The UI language (Settings::uiLanguage): Gifiles' own translations (embedded, Korean is the source
// text) and Qt's for its standard dialogs.
void installTranslations()
{
    const QString lang = Settings::uiLanguage();
    if (lang != QLatin1String("ko")) {
        auto *t = new QTranslator(qApp);
        if (t->load(QStringLiteral(":/i18n/gifiles_%1.qm").arg(lang)))
            QCoreApplication::installTranslator(t);
    }
    auto *qt = new QTranslator(qApp);
    if (qt->load(QStringLiteral("qtbase_%1").arg(lang), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qt);
}

// Handles the configuration commands without starting the GUI; returns -1 for a normal start.
int runCommand(int argc, char *argv[])
{
    if (argc < 2)
        return -1;
    const QString cmd = QString::fromLocal8Bit(argv[1]);
    if (cmd != QLatin1String("--help") && cmd != QLatin1String("-h") && cmd != QLatin1String("--config-path") &&
        cmd != QLatin1String("--print-default-config") && cmd != QLatin1String("--print-config") &&
        cmd != QLatin1String("--check-config") && cmd != QLatin1String("--version"))
        return -1;
    // Only the answer on stdout (and problems on stderr): no Qt log lines such as Linux's missing pipewire.
    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES"))
        qputenv("QT_LOGGING_RULES", "qt.*=false");
    QCoreApplication app(argc, argv);
    setNames();
    installTranslations();
    if (cmd == QLatin1String("--help") || cmd == QLatin1String("-h")) {
        print(Gifiles::tr(kHelp));
        return 0;
    }
    if (cmd == QLatin1String("--version")) {
        print(QStringLiteral("Gifiles %1\n").arg(QCoreApplication::applicationVersion()));
        return 0;
    }
    if (cmd == QLatin1String("--config-path")) {
        print(QDir::toNativeSeparators(Settings::configPath()) + QLatin1Char('\n'));
        return 0;
    }
    if (cmd == QLatin1String("--print-default-config")) {
        // Not Settings::instance(): that would create the file on a fresh install.
        print(Settings::renderDefaults());
        return 0;
    }
    if (cmd == QLatin1String("--print-config")) {
        QStringList problems;
        print(Settings::renderEffective(problems));
        for (const QString &p : problems)
            print(QStringLiteral("config.toml: %1\n").arg(p), true);
        return 0;
    }
    const QString path = argc > 2 ? QString::fromLocal8Bit(argv[2]) : Settings::configPath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        print(Gifiles::tr("%1: 읽을 수 없습니다 (없으면 앱이 처음 실행될 때 기본값으로 만듭니다)\n").arg(QDir::toNativeSeparators(path)));
        return 1;
    }
    const QStringList problems = Settings::check(QString::fromUtf8(f.readAll()));
    if (problems.isEmpty()) {
        print(Gifiles::tr("%1: 문제 없음\n").arg(QDir::toNativeSeparators(path)));
        return 0;
    }
    print(QStringLiteral("%1:\n").arg(QDir::toNativeSeparators(path)));
    for (const QString &p : problems)
        print(QStringLiteral("  %1\n").arg(p));
    return 1;
}

} // namespace

int main(int argc, char *argv[])
{
    if (const int code = runCommand(argc, argv); code >= 0)
        return code;
    PreviewWidget::chooseMediaBackend();
    QApplication app(argc, argv);
    setNames();
#ifdef Q_OS_LINUX
    QApplication::setDesktopFileName(QStringLiteral("dev.zidell.Gifiles"));
#endif
    installTranslations();
    QApplication::setApplicationDisplayName(QStringLiteral("Gifiles"));
    // Debug log while running from a checkout (the build machine): <source>/logs, gitignored.
    if (const QString src = QStringLiteral(GIFILES_SOURCE_DIR); QFileInfo::exists(src + QStringLiteral("/CMakeLists.txt"))) {
        Log::start(src + QStringLiteral("/logs"));
        Log::write("app", QStringLiteral("start %1 | args: %2 | config: %3")
                              .arg(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1).join(QLatin1Char(' ')),
                                   Settings::configPath()));
        QObject::connect(&app, &QCoreApplication::aboutToQuit, [] { Log::write("app", QStringLiteral("quit")); });
    }
#ifndef Q_OS_MACOS
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/gifiles.svg"))); // macOS: the bundle's .icns
#endif
    Theme::instance()->install();
#ifdef Q_OS_MACOS
    macFixAccessibilityHitTest();
#endif
#ifdef GIFILES_UPDATES
    Updater::allowUpdates();
#endif
    // A downloaded update goes in once the app has quit (Updater::apply starts the helper).
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] { Updater::instance()->apply(false); });
    Updater::instance();

    QStringList paths;
    for (int i = 1; i < argc; ++i) {
        QFileInfo fi(QString::fromLocal8Bit(argv[i]));
        if (fi.exists())
            paths << fi.absoluteFilePath();
    }
    if (paths.isEmpty() && Settings::instance()->flag(Settings::RestoreSession))
        App::instance()->restoreSession();
    else if (paths.isEmpty())
        App::instance()->newWindow();
    else
        App::instance()->newWindow(paths);
    // Development aid: GIFILES_SNAPSHOT=<dir> saves list/gallery/column renderings of the first
    // window (real platform rendering, unlike the offscreen tests) and quits.
    if (const QString dir = qEnvironmentVariable("GIFILES_SNAPSHOT"); !dir.isEmpty()) {
        QTimer::singleShot(1500, [dir] {
            MainWindow *w = nullptr;
            for (QWidget *tw : QApplication::topLevelWidgets())
                if ((w = qobject_cast<MainWindow *>(tw)))
                    break;
            if (!w)
                return QApplication::quit();
            const QString initialFolder = w->tab()->path();
            QDir().mkpath(dir);
            if (const QString focusPath = qEnvironmentVariable("GIFILES_SNAPSHOT_SIDEBAR_FOCUS"); !focusPath.isEmpty()) {
                if (auto *sidebar = w->findChild<Sidebar *>()) {
                    for (int section = 0; section < sidebar->topLevelItemCount(); ++section)
                        for (int i = 0; i < sidebar->topLevelItem(section)->childCount(); ++i) {
                            auto *item = sidebar->topLevelItem(section)->child(i);
                            if (item->data(0, Qt::UserRole + 1).toString() == focusPath)
                                sidebar->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
                        }
                    sidebar->setFocus();
                    w->grab().save(QDir(dir).filePath(QStringLiteral("sidebar-focus.png")));
                    w->tab()->focusView();
                    w->grab().save(QDir(dir).filePath(QStringLiteral("sidebar-unfocused.png")));
                }
            }
            const char *names[] = {"list", "gallery", "columns"};
            for (int m = 0; m < 3; ++m) {
                w->tab()->setMode(BrowserTab::Mode(m));
                w->tab()->focusView();
                if (m == 0 && w->tab()->itemCount() > 0) { // show a selected folder row
                    QAbstractItemView *v = w->tab()->view();
                    v->setCurrentIndex(v->model()->index(0, 0, v->rootIndex()));
                }
                if (m == 2 && w->tab()->itemCount() > 0)
                    w->tab()->view()->setCurrentIndex(w->tab()->view()->currentIndex());
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < 900)
                    QApplication::processEvents(QEventLoop::AllEvents, 50);
                w->grab().save(QDir(dir).filePath(QString::fromLatin1(names[m]) + QStringLiteral(".png")));
                if (w->tab()->itemCount() > 0) { // the same item with the rename editor open
                    w->tab()->renameSelected();
                    t.restart();
                    while (t.elapsed() < 400)
                        QApplication::processEvents(QEventLoop::AllEvents, 50);
                    w->grab().save(QDir(dir).filePath(QString::fromLatin1(names[m]) + QStringLiteral("-rename.png")));
                    if (QWidget *f = QApplication::focusWidget()) {
                        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                        QApplication::sendEvent(f, &esc);
                    }
                }
            }
            // The terminal panel with two tabs.
            for (QAction *a : w->actions())
                if (a->objectName() == QStringLiteral("터미널 펼치기"))
                    a->trigger();
            for (QToolButton *b : w->findChildren<QToolButton *>())
                if (b->toolTip().startsWith(Gifiles::tr("새로운 터미널 탭")))
                    b->click();
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 1500)
                QApplication::processEvents(QEventLoop::AllEvents, 50);
            w->grab().save(QDir(dir).filePath(QStringLiteral("terminal.png")));
            auto settle = [](int ms) {
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < ms)
                    QApplication::processEvents(QEventLoop::AllEvents, 50);
            };
            auto save = [dir](QWidget *widget, const char *name) {
                if (widget)
                    widget->grab().save(QDir(dir).filePath(QString::fromLatin1(name) + QStringLiteral(".png")));
            };
            auto top = [](const QString &name) -> QWidget * {
                for (QWidget *tw : QApplication::topLevelWidgets())
                    if (tw->isVisible() && (tw->objectName() == name || tw->metaObject()->className() == name))
                        return tw;
                return nullptr;
            };
            // The context menu on the first item (grabbed while it is open).
            w->tab()->setMode(BrowserTab::List);
            w->tab()->navigate(initialFolder);
            settle(400);
            if (w->tab()->itemCount() > 0) {
                QAbstractItemView *v = w->tab()->view();
                v->setCurrentIndex(v->model()->index(0, 0, v->rootIndex()));
                emit w->tab()->contextMenuRequested(w->mapToGlobal(QPoint(300, 200)), true);
            }
            // The one-line prompt of "AI로 실행", over the window.
            if (w->tab()->itemCount() > 0) {
                w->runSelectionCommand(Settings::instance()->selectionCommand(QStringLiteral("ai")), {w->tab()->currentItemPath()});
                settle(500);
                QPixmap shot = w->grab();
                if (auto *edit = w->findChild<QWidget *>(QStringLiteral("aiPrompt"))) {
                    QPainter painter(&shot);
                    painter.drawPixmap(w->mapFromGlobal(edit->window()->geometry().topLeft()), edit->window()->grab());
                    painter.end();
                    edit->window()->close();
                }
                shot.save(QDir(dir).filePath(QStringLiteral("prompt-window.png")));
            }
            // Settings: the selection menu, colors and shortcut pages, and their dialogs (nothing is saved).
            SettingsDialog::showSingleton(w, Gifiles::tr("선택 항목 메뉴"));
            settle(600);
            QWidget *settings = top(QStringLiteral("SettingsDialog"));
            save(settings, "settings-selection");
            if (auto *edit = settings ? settings->findChild<QPushButton *>(QStringLiteral("commandEdit")) : nullptr) {
                edit->click();
                settle(500);
                QWidget *d = top(QStringLiteral("commandEdit"));
                save(d, "command-edit");
                if (d)
                    d->close();
            }
            SettingsDialog::showSingleton(w, Gifiles::tr("모양 및 색상"));
            settle(500);
            save(settings, "settings-colors");
            SettingsDialog::showSingleton(w, Gifiles::tr("단축키"));
            settle(500);
            save(settings, "settings-shortcuts");
            if (auto *rebind = settings ? settings->findChild<QPushButton *>(QStringLiteral("rebind:열기")) : nullptr) {
                rebind->click();
                settle(500);
                if (QWidget *d = top(QStringLiteral("keyCapture"))) {
                    QKeyEvent k(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
                    QApplication::sendEvent(d, &k);
                    settle(300);
                    save(d, "key-capture");
                    d->close();
                }
            }
#ifdef Q_OS_MACOS
            // GIFILES_SNAPSHOT_QUICKLOOK=<file>: Quick Look on that file. The system's own preview is a
            // native view Qt's grab() can't see, so the window is captured by screencapture.
            if (const QString file = qEnvironmentVariable("GIFILES_SNAPSHOT_QUICKLOOK"); !file.isEmpty()) {
                auto *ql = new QuickLookWindow(w);
                ql->placeAround(w->geometry().center());
                ql->showPath(file);
                ql->present();
                settle(4000);
                QProcess::execute(QStringLiteral("/usr/sbin/screencapture"),
                                  {QStringLiteral("-x"), QStringLiteral("-o"), QStringLiteral("-l%1").arg(macWindowNumber(ql)),
                                   QDir(dir).filePath(QStringLiteral("quick-look.png"))});
                // The same Quick Look over the browser window, where it opened (quick-look-window.png).
                QPixmap shot = w->grab();
                QPixmap panel(QDir(dir).filePath(QStringLiteral("quick-look.png")));
                if (!panel.isNull()) {
                    panel.setDevicePixelRatio(shot.devicePixelRatio());
                    QPainter painter(&shot);
                    painter.drawPixmap(w->mapFromGlobal(ql->frameGeometry().topLeft()), panel);
                    painter.end();
                    shot.save(QDir(dir).filePath(QStringLiteral("quick-look-window.png")));
                }
                ql->close();
            }
#endif
            QApplication::quit();
        });
        return app.exec();
    }
    // Once the first window is up, offer to set up Full Disk Access (macOS) if it's missing.
    QTimer::singleShot(500, [] {
        if (const QStringList problems = Settings::instance()->problems(); !problems.isEmpty())
            App::instance()->showConfigProblems(problems);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<MainWindow *>(w) && w->isVisible()) {
                Permissions::showDialog(w, false);
                break;
            }
    });
    return app.exec();
}
