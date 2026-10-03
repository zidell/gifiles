#include "MainWindow.h"
#include "App.h"
#include "PathBar.h"
#include "Preview.h"
#include "Sidebar.h"
#include "OpenWith.h"
#include "Permissions.h"
#include "Log.h"
#include "Settings.h"
#include "Shortcuts.h"
#include "TerminalWidget.h"
#include "Updater.h"
#include "Theme.h"
#include "Util.h"
#ifdef Q_OS_MACOS
#include "MacWindow.h"
#endif

#include <QAbstractItemView>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFileIconProvider>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QTransform>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent>

namespace {

#ifdef Q_OS_MACOS
constexpr int kTitleBarHeight = 52; // content runs under the transparent title bar
#else
constexpr int kTitleBarHeight = 46;
#endif

const QString kCutMime = QStringLiteral("application/x-gifiles-cut");
const QString kWinDropEffect = QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");

class AiPromptPopup final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::WindowDeactivate) {
            // A tool window supports IME on Windows; restore Popup's expected click-away close.
            QTimer::singleShot(0, this, [this] {
                if (!isActiveWindow())
                    close();
            });
        }
        return QWidget::event(e);
    }
};

BrowserTab::Mode defaultMode()
{
    const QString mode = Settings::instance()->value(Settings::DefaultMode).toString();
    return mode == QLatin1String("gallery") ? BrowserTab::Gallery : mode == QLatin1String("columns") ? BrowserTab::Columns : BrowserTab::List;
}

QStringList localPaths(const QList<QUrl> &urls)
{
    QStringList out;
    for (const QUrl &u : urls)
        if (u.isLocalFile())
            out << QDir::cleanPath(u.toLocalFile());
    return out;
}

} // namespace

MainWindow::MainWindow(const QStringList &tabPaths, QWidget *parent) : QMainWindow(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    // We lay out under the title bar ourselves (traffic lights over the sidebar).
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    resize(1180, 720);

    m_sidebar = new Sidebar(this);
    m_sidebar->installEventFilter(this);
    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setTabBarAutoHide(true);
    m_tabs->tabBar()->setExpanding(true);
    m_tabs->tabBar()->setDrawBase(false);
    m_previewPane = new PreviewWidget(PreviewWidget::Pane, this);
    m_previewPane->hide();

    createActions();
    createMenus();
    createToolbar();
    createStatusBar();

    // Finder layout: the sidebar runs the full window height (under the traffic lights on macOS);
    // toolbar, tabs and status bar belong to the content column.
    m_sidebarPanel = new QWidget(this);
    m_sidebarPanel->setObjectName(QStringLiteral("sidebarPanel"));
    m_sidebarPanel->setAttribute(Qt::WA_StyledBackground);
    auto *left = new QVBoxLayout(m_sidebarPanel);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(0);
    m_sidebarTop = new QWidget(m_sidebarPanel);
    m_sidebarTop->setFixedHeight(kTitleBarHeight);
    m_sidebarTop->installEventFilter(this);
    left->addWidget(m_sidebarTop);
    left->addWidget(m_sidebar, 1);

    auto *content = new QSplitter(Qt::Horizontal, this);
    content->addWidget(m_tabs);
    content->addWidget(m_previewPane);
    content->setStretchFactor(0, 1);
    content->setCollapsible(0, false);
    content->setHandleWidth(1);
    // Terminal panel under the files. Its header bar always stays; ⌃`, the chevron or a click on
    // the bar folds the terminal down to the bar and opens it again. Height: drag the top edge.
    m_vsplit = new QSplitter(Qt::Vertical, this);
    m_vsplit->setObjectName(QStringLiteral("vsplit"));
    m_vsplit->addWidget(content);
    m_vsplit->setCollapsible(0, false);
    m_vsplit->setHandleWidth(5);
    m_termPanel = new QWidget(this);
    auto *tl = new QVBoxLayout(m_termPanel);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(0);
    auto *termHeader = new QWidget(m_termPanel);
    termHeader->setObjectName(QStringLiteral("termHeader"));
    termHeader->setAttribute(Qt::WA_StyledBackground);
    termHeader->setFixedHeight(kTermHeaderHeight);
    termHeader->installEventFilter(this);
    m_termHeader = termHeader;
    auto *hl = new QHBoxLayout(termHeader);
    hl->setContentsMargins(12, 0, 6, 0);
    m_termTitle = new QLabel(termHeader);
    m_termTitle->setObjectName(QStringLiteral("secondary"));
    m_termTitle->setText(Gifiles::tr("터미널"));
    hl->addWidget(m_termTitle);
    m_termTabs = new QTabBar(termHeader);
    m_termTabs->setObjectName(QStringLiteral("termTabs"));
    m_termTabs->setFocusPolicy(Qt::NoFocus);
    m_termTabs->setDrawBase(false);
    m_termTabs->setExpanding(false);
    m_termTabs->setTabsClosable(true);
    m_termTabs->setMovable(true);
    m_termTabs->setElideMode(Qt::ElideRight);
    m_termTabs->setUsesScrollButtons(true);
    m_termTabs->hide();
    hl->addWidget(m_termTabs);
    auto *termAdd = new QToolButton(termHeader);
    m_termAdd = termAdd;
    termAdd->hide(); // only while the terminal is open
    termAdd->setObjectName(QStringLiteral("flat"));
    termAdd->setFocusPolicy(Qt::NoFocus);
#ifdef Q_OS_MACOS
    termAdd->setToolTip(Gifiles::tr("새로운 터미널 탭 (터미널에서 ⌘T)"));
#else
    termAdd->setToolTip(Gifiles::tr("새로운 터미널 탭 (터미널에서 Ctrl+Shift+T)"));
#endif
    connect(termAdd, &QToolButton::clicked, this, [this] {
        addTerminal(tab()->path());
        setTerminalVisible(true);
    });
    m_themed << qMakePair(QPointer<QToolButton>(termAdd), QStringLiteral("plus"));
    hl->addWidget(termAdd);
    hl->addStretch(1);
    m_termToggle = new QToolButton(termHeader);
    m_termToggle->setObjectName(QStringLiteral("flat"));
    m_termToggle->setFocusPolicy(Qt::NoFocus);
    connect(m_termToggle, &QToolButton::clicked, this, [this] { setTerminalVisible(!m_terminalAct->isChecked()); });
    m_themed << qMakePair(QPointer<QToolButton>(m_termToggle), QStringLiteral("chevron-up-small"));
    hl->addWidget(m_termToggle);
    tl->addWidget(termHeader);
    m_termStack = new QStackedWidget(m_termPanel);
    m_termStack->setObjectName(QStringLiteral("termStack"));
    m_termStack->hide();
    tl->addWidget(m_termStack, 1);
    tl->addStretch(); // keeps the bar on top while the terminal is folded
    connect(m_termTabs, &QTabBar::currentChanged, this, [this](int i) {
        if (i < 0)
            return;
        const bool focus = terminalHasFocus(); // a tab opened for the browser never takes the keyboard
        m_termStack->setCurrentIndex(i);
        TerminalWidget *t = term();
        if (focus)
            t->setFocus();
        // Switching tabs shows that shell's folder in the list ("one tool").
        if (Settings::instance()->flag(Settings::TermSyncBack) && !t->shellCwd().isEmpty() && !t->isAt(tab()->path()))
            tab()->navigate(t->shellCwd());
    });
    connect(m_termTabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        QWidget *w = m_termStack->widget(from);
        m_termStack->removeWidget(w);
        m_termStack->insertWidget(to, w);
        m_termStack->setCurrentIndex(m_termTabs->currentIndex());
    });
    connect(m_termTabs, &QTabBar::tabCloseRequested, this, &MainWindow::closeTerminal);
    connect(m_termTabs, &QTabBar::tabBarClicked, this, [this](int i) {
        if (i < 0)
            return;
        if (!m_termOpen)
            setTerminalVisible(true);
        QTimer::singleShot(0, this, [this] {
            if (term() && term()->isVisible())
                term()->setFocus();
        });
    });
    m_termPanel->setMinimumHeight(kTermHeaderHeight); // the splitter may shrink it to the bar
    m_vsplit->addWidget(m_termPanel); // starts folded: just the bar
    // Dragging the edge of a folded terminal up unfolds it at that height; dragging an open one
    // down to the bar folds it.
    connect(m_vsplit, &QSplitter::splitterMoved, this, [this] {
        const int h = m_vsplit->sizes().value(1);
        if (!m_terminalAct->isChecked() && h > kTermHeaderHeight + 12)
            setTerminalVisible(true, true);
        else if (m_terminalAct->isChecked() && h <= kTermHeaderHeight + 4)
            setTerminalVisible(false);
    });
    m_vsplit->setStretchFactor(0, 1);
    m_vsplit->setStretchFactor(1, 0);

    auto *right = new QWidget(this);
    auto *rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    rl->addWidget(m_toolbar);
    rl->addWidget(m_vsplit, 1);
    rl->addWidget(m_statusStrip);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(m_sidebarPanel);
    m_splitter->addWidget(right);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(1, false);
    m_splitter->setHandleWidth(1);
    m_splitter->setSizes({210, 970});
    setCentralWidget(m_splitter);
#if defined(Q_OS_MACOS) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    // Content runs under a transparent title bar; the traffic lights float over the sidebar.
    setWindowFlag(Qt::ExpandedClientAreaHint);
    setWindowFlag(Qt::NoTitleBarBackgroundHint);
#endif
    updateToolbarInset();
    refreshIcons();
    connect(Theme::instance(), &Theme::changed, this, &MainWindow::refreshIcons);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_sidebar, &Sidebar::rightPressed, this, [this] { tab()->focusView(); });
    connect(m_sidebar, &Sidebar::placeActivated, this, [this](const QString &p) {
        go(p);
        tab()->focusView();
    });
    connect(m_sidebar, &Sidebar::openInNewTab, this, [this](const QString &p) { addTab(p); });
    connect(m_sidebar, &Sidebar::dropRequested, this, &MainWindow::handleDrop);
    connect(m_pathBar, &PathBar::pathActivated, this, &MainWindow::go);
    connect(m_pathBar, &PathBar::editingFinished, this, [this] { tab()->focusView(); });
    connect(App::instance(), &App::showHiddenChanged, this, [this](bool show) {
        m_showHidden->setChecked(show);
        for (int i = 0; i < m_tabs->count(); ++i)
            static_cast<BrowserTab *>(m_tabs->widget(i))->setShowHidden(show);
    });
    connect(App::instance(), &App::undoChanged, this, &MainWindow::updateUndoActions);
    connect(App::instance(), &App::cutChanged, this, [this] {
        for (QAbstractItemView *v : m_tabs->findChildren<QAbstractItemView *>())
            v->viewport()->update();
    });
    updateUndoActions();

    for (const QString &p : tabPaths) // in their order (a restored session, several folders)
        addTab(p, false, true);
    m_tabs->setCurrentIndex(0);
    onTabChanged();
}

// ---------------------------------------------------------------------------
// Actions & menus

QAction *MainWindow::act(const QString &text, std::function<void()> fn)
{
    // `text` is the Korean menu title: the action's id (built-in keys in Shortcuts.cpp, the user's in
    // config.toml); the menu shows it translated.
    auto *a = new QAction(Gifiles::tr(text.toUtf8().constData()), this);
    a->setObjectName(text);
    Shortcuts::instance()->add(a, text);
    connect(a, &QAction::triggered, this, [this, a, fn] {
        if (!Log::enabled())
            return fn();
        const QString key = Log::recentKey();
        Log::write("action", QStringLiteral("%1 (%2) | %3").arg(a->text(), key.isEmpty() ? QStringLiteral("menu/click") : key, debugState()));
        fn();
        Log::write("after", debugState());
    });
    if (Shortcuts::instance()->context(text) == QLatin1String("files"))
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    else
        addAction(a); // window shortcuts work even when the menu bar is hidden

    return a;
}

QAction *MainWindow::act(const QString &text, void (MainWindow::*slot)())
{
    return act(text, [this, slot] { (this->*slot)(); });
}

void MainWindow::createActions()
{
    m_newWindowAct = act(QStringLiteral("새로운 윈도우"), [this] {
                             const bool home = Settings::instance()->flag(Settings::NewWindowHome);
                             App::instance()->newWindow({home ? QDir::homePath() : tab()->path()}, this);
                         });
    // ⌘T / ⌘W / tab switching act on the terminal's tabs while it has the keyboard.
    m_newTabAct = act(QStringLiteral("새로운 탭"), [this] {
        if (terminalHasFocus())
            addTerminal(tab()->path())->setFocus();
        else
            addTab(tab()->path());
    });
    m_newFolderAct = act(QStringLiteral("새로운 폴더"), &MainWindow::newFolder);
    m_newFolderWithAct = act(QStringLiteral("선택 항목으로 새로운 폴더"), &MainWindow::newFolderWithSelection);

    // On selected files the open key shows the context menu (open is its first, highlighted item:
    // Return opens): doing something else with them is more common than opening. A single folder
    // is entered at once.
    m_openAct = act(QStringLiteral("열기"), [this] {
                        const QStringList sel = tab()->selectedPaths();
                        const QFileInfo fi(sel.value(0));
                        if (sel.size() == 1 && fi.isDir() && !util::isPackage(fi))
                            tab()->openSelection(false);
                        else if (!sel.isEmpty())
                            showContextMenu(tab()->selectionMenuPos(), true, true);
                    });
    m_openWithAct = act(QStringLiteral("다음으로 열기…"), [this] {
        QStringList files;
        for (const QString &p : tab()->selectedPaths())
            if (!QFileInfo(p).isDir())
                files << p;
        OpenWith::showDialog(this, files);
    });
    m_openTabAct = act(QStringLiteral("새로운 탭에서 열기"), [this] { tab()->openSelection(true); });
    m_closeTabAct = act(QStringLiteral("탭 닫기"), [this] {
                            if (closeFrontWindow())
                                return;
                            if (terminalHasFocus())
                                closeTerminal(m_termTabs->currentIndex());
                            else
                                closeTab(m_tabs->currentIndex());
                        });
    m_closeWindowAct = act(QStringLiteral("윈도우 닫기"), [this] {
        if (!closeFrontWindow())
            close();
    });
    m_infoAct = act(QStringLiteral("정보 가져오기"), &MainWindow::showInfo);
    // The context menus offer this in place of 정보 가져오기 (still in the File menu, ⌘I).
    m_revealAct = act(util::revealActionId(), &MainWindow::revealInFileManager);
    m_renameAct = act(QStringLiteral("이름 변경"), [this] { tab()->renameSelected(); });
    m_duplicateAct = act(QStringLiteral("복제"), &MainWindow::duplicate);
    m_quickLookAct = act(QStringLiteral("퀵 뷰어"), &MainWindow::toggleQuickLook);
    m_trashAct = act(QStringLiteral("휴지통으로 이동"), &MainWindow::moveToTrash);
    m_permissionsAct = act(QStringLiteral("전체 디스크 접근 권한…"), [this] { Permissions::showDialog(this, true); });
    m_settingsAct = act(QStringLiteral("설정…"), [this] { SettingsDialog::showSingleton(this); });
    m_settingsAct->setMenuRole(QAction::PreferencesRole);
    m_updateAct = act(QStringLiteral("업데이트 확인…"), &MainWindow::checkForUpdates);
    m_updateAct->setMenuRole(QAction::ApplicationSpecificRole); // macOS: in the app menu
    m_quitAct = act(QStringLiteral("종료"), [] { App::instance()->quit(); });
    m_quitAct->setMenuRole(QAction::QuitRole);

    m_undo = act(QStringLiteral("실행 취소"), &MainWindow::undo);
    m_redo = act(QStringLiteral("실행 복귀"), &MainWindow::redo);
    m_cutAct = act(QStringLiteral("잘라내기"), [this] { copyToClipboard(true); });
    m_copyAct = act(QStringLiteral("복사하기"), [this] { copyToClipboard(false); });
    m_pasteAct = act(QStringLiteral("붙여넣기"), [this] { paste(false); });
    m_moveHereAct = act(QStringLiteral("항목을 여기로 이동"), [this] { paste(true); });
    m_copyPathAct = act(QStringLiteral("경로 복사"), &MainWindow::copyPath);
    m_selectAllAct = act(QStringLiteral("전체 선택"), [this] { tab()->selectAll(); });
    m_findAct = act(QStringLiteral("찾기"), [this] {
        m_search->setFocus();
        m_search->selectAll();
    });

    m_modeGroup = new QActionGroup(this);
    // Same numbers as Finder: ⌘1 icons (gallery here), ⌘2 list, ⌘3 columns.
    m_modeGallery = act(QStringLiteral("갤러리로"), [this] { setMode(BrowserTab::Gallery); });
    m_modeList = act(QStringLiteral("목록으로"), [this] { setMode(BrowserTab::List); });
    m_modeColumns = act(QStringLiteral("컬럼으로"), [this] { setMode(BrowserTab::Columns); });
    m_modeList->setIconText(QStringLiteral("☰"));
    m_modeGallery->setIconText(QStringLiteral("⊞"));
    m_modeColumns->setIconText(QStringLiteral("▥"));
    for (QAction *a : {m_modeGallery, m_modeList, m_modeColumns}) {
        a->setCheckable(true);
        m_modeGroup->addAction(a);
    }

    m_showHidden = act(QStringLiteral("숨김 파일 보기"), [this] { App::instance()->setShowHidden(m_showHidden->isChecked()); });
    m_showHidden->setCheckable(true);
    m_showHidden->setChecked(App::instance()->showHidden());
    m_showSidebar = act(QStringLiteral("사이드바 보기"), [this] {
                            m_sidebarPanel->setVisible(m_showSidebar->isChecked());
                            updateToolbarInset();
                        });
    m_showSidebar->setCheckable(true);
    m_showSidebar->setChecked(true);
    m_showPreview = act(QStringLiteral("미리보기 보기"), [this] {
        m_previewPane->setVisible(m_showPreview->isChecked());
        if (m_previewPane->isVisible() && tab()) // it isn't updated while hidden: show the current item now
            m_previewPane->setPath(tab()->currentItemPath().isEmpty() ? tab()->path() : tab()->currentItemPath());
        updateStatus();
    });
    m_showPreview->setCheckable(true);
    // The Korean input source turns the ` key into ₩; Shortcuts accepts both.
    m_terminalAct = act(QStringLiteral("터미널 펼치기"), [this] { setTerminalVisible(m_terminalAct->isChecked()); });
    m_terminalAct->setCheckable(true);
    m_focusSidebarAct = act(QStringLiteral("사이드바로 이동"), [this] {
        if (!m_sidebarPanel->isVisible())
            m_showSidebar->trigger();
        focusSidebar();
    });
    m_focusFilesAct = act(QStringLiteral("파일뷰로 이동"), [this] { tab()->focusView(); });
    m_focusTerminalAct = act(QStringLiteral("터미널로 이동"), [this] {
        if (!m_termOpen)
            setTerminalVisible(true);
        else if (term())
            term()->setFocus();
    });
    // ⌘+ / ⌘− / ⌘0: the file views' text size (config.toml view.font_size, 0 = system size).
    auto fontStep = [](int by) {
        Settings *s = Settings::instance();
        const int cur = s->value(Settings::FontSize).toInt();
        const int base = cur > 0 ? cur : QApplication::font().pointSize();
        s->setValue(Settings::FontSize, by == 0 ? 0 : qBound(9, base + by, 32));
    };
    m_fontBiggerAct = act(QStringLiteral("글꼴 크게"), [fontStep] { fontStep(1); });
    m_fontSmallerAct = act(QStringLiteral("글꼴 작게"), [fontStep] { fontStep(-1); });
    m_fontResetAct = act(QStringLiteral("기본 글꼴 크기"), [fontStep] { fontStep(0); });
    m_biggerAct = act(QStringLiteral("아이콘 크게"), [this] { m_iconSlider->setValue(m_iconSlider->value() + 16); });
    m_smallerAct = act(QStringLiteral("아이콘 작게"), [this] { m_iconSlider->setValue(m_iconSlider->value() - 16); });

    m_back = act(QStringLiteral("뒤로"), [this] { tab()->goBack(); });
    m_forward = act(QStringLiteral("앞으로"), [this] { tab()->goForward(); });
    m_up = act(QStringLiteral("상위 폴더"), [this] { tab()->goUp(); });
    m_gotoAct = act(QStringLiteral("폴더로 이동…"), [this] { m_pathBar->startEditing(); });

    auto place = [this](const QString &name, QStandardPaths::StandardLocation loc) {
        const QString p = QStandardPaths::writableLocation(loc);
        m_goPlaces << act(name, [this, p] { go(p); });
    };
    place(QStringLiteral("홈"), QStandardPaths::HomeLocation);
    place(QStringLiteral("데스크탑"), QStandardPaths::DesktopLocation);
    place(QStringLiteral("문서"), QStandardPaths::DocumentsLocation);
    place(QStringLiteral("다운로드"), QStandardPaths::DownloadLocation);
#ifdef Q_OS_MACOS
    m_goPlaces << act(QStringLiteral("응용 프로그램"), [this] { go(QStringLiteral("/Applications")); });
#endif

    m_nextTabAct = act(QStringLiteral("다음 탭 보기"), [this] {
                           if (terminalHasFocus())
                               m_termTabs->setCurrentIndex((m_termTabs->currentIndex() + 1) % m_termTabs->count());
                           else
                               m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
                       });
    m_prevTabAct = act(QStringLiteral("이전 탭 보기"), [this] {
                           if (terminalHasFocus())
                               m_termTabs->setCurrentIndex((m_termTabs->currentIndex() + m_termTabs->count() - 1) % m_termTabs->count());
                           else
                               m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
                       });
}

void MainWindow::createMenus()
{
    QMenu *file = menuBar()->addMenu(Gifiles::tr("파일"));
    file->addActions({m_newWindowAct, m_newTabAct, m_newFolderAct, m_newFolderWithAct, m_openAct, m_openTabAct});
    // Only folders open in a tab: greyed out while the menu shows files alone (its key still works,
    // it just skips files).
    connect(file, &QMenu::aboutToShow, this, [this] { m_openTabAct->setEnabled(tab() && tab()->hasSelectedFolder()); });
    connect(file, &QMenu::aboutToHide, this, [this] { m_openTabAct->setEnabled(true); });
    file->addSeparator();
    file->addActions({m_closeTabAct, m_closeWindowAct});
    file->addSeparator();
    file->addActions({m_infoAct, m_revealAct, m_renameAct, m_duplicateAct, m_quickLookAct});
    file->addSeparator();
    file->addAction(m_trashAct);
    file->addSeparator();
    file->addAction(m_settingsAct);
    file->addAction(m_updateAct);
    file->addAction(m_quitAct);

    QMenu *edit = menuBar()->addMenu(Gifiles::tr("편집"));
    edit->addActions({m_undo, m_redo});
    edit->addSeparator();
    edit->addActions({m_cutAct, m_copyAct, m_pasteAct, m_moveHereAct, m_copyPathAct, m_selectAllAct});
    edit->addSeparator();
    edit->addAction(m_findAct);

    QMenu *view = menuBar()->addMenu(Gifiles::tr("보기"));
    view->addActions({m_modeGallery, m_modeList, m_modeColumns});
    view->addSeparator();
    view->addActions({m_showHidden, m_showSidebar, m_showPreview, m_terminalAct});
    view->addSeparator();
    view->addActions({m_focusSidebarAct, m_focusFilesAct, m_focusTerminalAct});
    view->addSeparator();
    view->addActions({m_fontBiggerAct, m_fontSmallerAct, m_fontResetAct, m_biggerAct, m_smallerAct});

    QMenu *go = menuBar()->addMenu(Gifiles::tr("이동"));
    go->addActions({m_back, m_forward, m_up});
    go->addSeparator();
    go->addActions(m_goPlaces);
    go->addSeparator();
    go->addAction(m_gotoAct);

    QMenu *win = menuBar()->addMenu(Gifiles::tr("윈도우"));
    win->addAction(act(QStringLiteral("최소화"), [this] { showMinimized(); }));
    win->addSeparator();
    win->addActions({m_nextTabAct, m_prevTabAct});
    win->addSeparator();
    win->addAction(m_permissionsAct);
    m_menus = {file, edit, view, go, win};
#ifndef Q_OS_MACOS
    menuBar()->hide(); // reachable from the toolbar's "⋯" button; shortcuts stay active
#endif
}

QToolButton *MainWindow::flatButton(QAction *action, const QString &glyph, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setObjectName(QStringLiteral("flat"));
    b->setDefaultAction(action);
    b->setIconSize(QSize(18, 18));
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    b->setFocusPolicy(Qt::NoFocus);
    auto updateTip = [b, action] {
        QString tip = action->text();
        if (!action->shortcut().isEmpty())
            tip += QStringLiteral("  ") + action->shortcut().toString(QKeySequence::NativeText);
        b->setToolTip(tip);
    };
    updateTip();
    connect(action, &QAction::changed, b, updateTip); // custom shortcuts, 펼치기/접기
    m_themed << qMakePair(QPointer<QToolButton>(b), glyph);
    return b;
}

void MainWindow::createToolbar()
{
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("mainToolbar"));
    m_toolbar->setAttribute(Qt::WA_StyledBackground);
    m_toolbar->setFixedHeight(kTitleBarHeight);
    m_toolbar->installEventFilter(this); // drag the window by empty toolbar space
    auto *l = new QHBoxLayout(m_toolbar);
    l->setContentsMargins(10, 0, 12, 0);
    l->setSpacing(4);
    m_toolbarInset = new QWidget(m_toolbar); // keeps clear of the traffic lights when the sidebar is hidden
    m_toolbarInset->setFixedWidth(0);
    l->addWidget(m_toolbarInset);
    l->addWidget(flatButton(m_showSidebar, QStringLiteral("sidebar"), m_toolbar));
    l->addSpacing(4);
    l->addWidget(flatButton(m_back, QStringLiteral("chevron-left"), m_toolbar));
    l->addWidget(flatButton(m_forward, QStringLiteral("chevron-right"), m_toolbar));
    l->addSpacing(6);
    m_pathBar = new PathBar(m_toolbar);
    l->addWidget(m_pathBar, 1);
    l->addSpacing(8);

    auto *seg = new QFrame(m_toolbar);
    seg->setObjectName(QStringLiteral("segmented"));
    seg->setFixedHeight(32);
    auto *sl = new QHBoxLayout(seg);
    sl->setContentsMargins(2, 2, 2, 2);
    sl->setSpacing(2);
    const QStringList glyphs{QStringLiteral("grid"), QStringLiteral("list"), QStringLiteral("columns")};
    const QList<QAction *> modes{m_modeGallery, m_modeList, m_modeColumns};
    for (int i = 0; i < 3; ++i) {
        QToolButton *b = flatButton(modes[i], glyphs[i], seg);
        b->setObjectName(QString());
        sl->addWidget(b);
    }
    l->addWidget(seg);
    l->addSpacing(4);
    l->addWidget(flatButton(m_showPreview, QStringLiteral("preview"), m_toolbar));
#ifndef Q_OS_MACOS
    // Windows/Linux: menus live behind one button instead of an old-style menu bar.
    auto *more = new QToolButton(m_toolbar);
    more->setObjectName(QStringLiteral("flat"));
    more->setIconSize(QSize(18, 18));
    more->setPopupMode(QToolButton::InstantPopup);
    more->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(more);
    for (QMenu *m : m_menus)
        menu->addMenu(m);
    more->setMenu(menu);
    m_themed << qMakePair(QPointer<QToolButton>(more), QStringLiteral("more"));
    l->addWidget(more);
#endif
    l->addSpacing(6);

    m_updateButton = new QToolButton(m_toolbar);
    m_updateButton->setObjectName(QStringLiteral("flat"));
    m_updateButton->setText(Gifiles::tr("업데이트"));
    m_updateButton->setFocusPolicy(Qt::NoFocus);
    connect(m_updateButton, &QToolButton::clicked, this, &MainWindow::restartToUpdate);
    l->addWidget(m_updateButton);
    const auto showUpdate = [this] {
        const QString v = Updater::instance()->readyVersion();
        m_updateButton->setVisible(!v.isEmpty());
        m_updateButton->setToolTip(Gifiles::tr("Gifiles %1을(를) 받아 두었습니다. 누르면 다시 시작해 설치합니다 (앱을 끌 때도 설치됩니다).").arg(v));
    };
    connect(Updater::instance(), &Updater::stateChanged, m_updateButton, showUpdate);
    showUpdate();

    m_search = new QLineEdit(m_toolbar);
    m_search->setObjectName(QStringLiteral("search"));
    m_search->setPlaceholderText(Gifiles::tr("검색"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(210);
    m_search->setFixedHeight(30);
    m_searchIcon = m_search->addAction(QIcon(), QLineEdit::LeadingPosition);
    l->addWidget(m_search);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        tab()->setSearch(t);
        updateStatus();
    });
    auto *esc = new QAction(m_search);
    esc->setShortcutContext(Qt::WidgetShortcut);
    Shortcuts::instance()->add(esc, QStringLiteral("검색 취소"));
    connect(esc, &QAction::triggered, this, [this] {
        m_search->clear();
        tab()->focusView();
    });
    m_search->addAction(esc);
    auto *down = new QAction(m_search);
    down->setShortcutContext(Qt::WidgetShortcut);
    Shortcuts::instance()->add(down, QStringLiteral("검색에서 파일뷰로 이동"));
    connect(down, &QAction::triggered, this, [this] { tab()->focusView(true); });
    m_search->addAction(down);

    QToolButton *plus = flatButton(m_newTabAct, QStringLiteral("plus"), m_tabs);
    m_tabs->setCornerWidget(plus, Qt::TopRightCorner);
}

void MainWindow::refreshIcons()
{
    // Icons go on the actions: a button's own icon is overwritten by its default action.
    for (auto &[button, glyph] : m_themed) {
        if (!button)
            continue;
        if (QAction *a = button->defaultAction())
            a->setIcon(Theme::icon(glyph));
        else
            button->setIcon(Theme::icon(glyph));
    }
    m_searchIcon->setIcon(Theme::icon(QStringLiteral("search"), Theme::colors().secondary, 16));
    // Menu icons (context menu and menu bar).
    const QList<QPair<QAction *, const char *>> menuIcons = {
        {m_openAct, "open"}, {m_openWithAct, "window"}, {m_openTabAct, "tab"}, {m_quickLookAct, "eye"},
        {m_infoAct, "info"}, {m_revealAct, "folder"}, {m_renameAct, "pencil"}, {m_duplicateAct, "copy"}, {m_newFolderWithAct, "folder-plus"},
        {m_newFolderAct, "folder-plus"}, {m_copyPathAct, "link"}, {m_cutAct, "scissors"}, {m_copyAct, "copy"},
        {m_pasteAct, "clipboard"}, {m_moveHereAct, "move"}, {m_trashAct, "trash"}, {m_newTabAct, "tab"},
        {m_newWindowAct, "window"}, {m_findAct, "search"}, {m_terminalAct, "terminal"}, {m_showHidden, "eye"},
        {m_modeList, "list"}, {m_modeGallery, "grid"}, {m_modeColumns, "columns"}, {m_gotoAct, "move"},
    };
    QSet<QAction *> onToolbar; // these already got their (larger) toolbar glyph above
    for (auto &[button, glyph] : m_themed)
        if (button && button->defaultAction())
            onToolbar << button->defaultAction();
    for (const auto &[a, glyph] : menuIcons) {
        if (!onToolbar.contains(a))
            a->setIcon(Theme::icon(QString::fromLatin1(glyph), {}, 16));
        a->setIconVisibleInMenu(true);
    }
    QTransform rot; // a "+" turned 45° is the cancel cross
    rot.rotate(45);
    m_cancel->setIcon(QIcon(Theme::icon(QStringLiteral("plus"), Theme::colors().secondary, 16)
                                .pixmap(QSize(16, 16), devicePixelRatio())
                                .transformed(rot, Qt::SmoothTransformation)));
}

void MainWindow::updateToolbarInset()
{
#ifdef Q_OS_MACOS
    // With the sidebar hidden the traffic lights sit over the toolbar.
    m_toolbarInset->setFixedWidth(m_showSidebar->isChecked() ? 0 : 70);
#endif
}

bool MainWindow::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_sidebar && ev->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        for (const QString &id : {QStringLiteral("사이드바로 이동"), QStringLiteral("파일뷰로 이동"), QStringLiteral("터미널로 이동")})
            if (Shortcuts::instance()->matches(id, ke)) {
                ke->ignore();
                return true;
            }
        for (const QString &id : {QStringLiteral("사이드바 항목 열기"), QStringLiteral("사이드바에서 파일뷰로 이동")})
            if (Shortcuts::instance()->matches(id, ke)) {
                ke->accept();
                return true;
            }
    }
    if (obj == m_termHeader && ev->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(ev)->button() == Qt::LeftButton) {
        setTerminalVisible(!m_terminalAct->isChecked());
        return true;
    }
    if (obj == m_toolbar || obj == m_sidebarTop) {
        if (ev->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(ev)->button() == Qt::LeftButton) {
            windowHandle()->startSystemMove();
            return true;
        }
        if (ev->type() == QEvent::MouseButtonDblClick) {
            isMaximized() ? showNormal() : showMaximized();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

void MainWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
#ifdef Q_OS_MACOS
    macHideTitleText(this);
#endif
}

void MainWindow::showMessage(const QString &text, int ms)
{
    m_statusLabel->setText(text);
    m_messageTimer->start(ms);
}

void MainWindow::createStatusBar()
{
    m_statusStrip = new QWidget(this);
    m_statusStrip->setObjectName(QStringLiteral("statusStrip"));
    m_statusStrip->setAttribute(Qt::WA_StyledBackground);
    m_statusStrip->setFixedHeight(28);
    auto *l = new QHBoxLayout(m_statusStrip);
    l->setContentsMargins(12, 0, 12, 0);
    l->setSpacing(8);
    m_statusLabel = new QLabel(m_statusStrip);
    m_statusLabel->setObjectName(QStringLiteral("secondary"));
    l->addWidget(m_statusLabel, 1);
    m_progress = new QProgressBar(m_statusStrip);
    m_progress->setFixedWidth(140);
    m_progress->setTextVisible(false);
    m_progress->hide();
    m_cancel = new QToolButton(m_statusStrip);
    m_cancel->setObjectName(QStringLiteral("flat"));
    m_cancel->setToolTip(Gifiles::tr("중단"));
    m_cancel->hide();
    m_iconSlider = new QSlider(Qt::Horizontal, m_statusStrip);
    m_iconSlider->setRange(32, 256);
    m_iconSlider->setValue(Settings::instance()->value(Settings::IconSize).toInt());
    m_iconSlider->setFixedWidth(110);
    m_iconSlider->setToolTip(Gifiles::tr("아이콘 크기"));
    connect(m_iconSlider, &QSlider::valueChanged, this, [this](int v) {
        tab()->setIconSize(v, true); // remembered for this folder
        Settings::instance()->setValue(Settings::IconSize, v); // default for new tabs
    });
    l->addWidget(m_progress);
    l->addWidget(m_cancel);
    l->addWidget(m_iconSlider);
    m_messageTimer = new QTimer(this);
    m_messageTimer->setSingleShot(true);
    connect(m_messageTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
}

// ---------------------------------------------------------------------------
// Tabs

BrowserTab *MainWindow::tab() const
{
    return static_cast<BrowserTab *>(m_tabs->currentWidget());
}

BrowserTab *MainWindow::addTab(const QString &path, bool activate, bool atEnd)
{
    BrowserTab::Mode mode = tab() ? tab()->mode() : defaultMode();
    auto *t = new BrowserTab(path, mode, App::instance()->showHidden(), m_tabs);
    t->setIconSize(m_iconSlider->value());
    const int at = atEnd ? -1 : m_tabs->currentIndex() + 1;
    const int i = m_tabs->insertTab(at < 1 ? m_tabs->count() : at, t, QString());
    connectTab(t);
    updateTabTitle(t);
    if (activate) {
        m_tabs->setCurrentIndex(i);
        t->focusView();
    }
    return t;
}

void MainWindow::connectTab(BrowserTab *t)
{
    for (QAction *a : {m_renameAct, m_quickLookAct, m_openAct})
        t->addAction(a);
    auto current = [this, t] { return t == tab(); };
    connect(t, &BrowserTab::pathChanged, this, [this, t, current] {
        Log::write("navigate", QStringLiteral("%1 (%2)").arg(t->path(), current() ? QStringLiteral("current tab") : QStringLiteral("other tab")));
        updateTabTitle(t);
        if (current()) {
            updateNav();
            updateStatus();
            terminalFollow(t->path());
            forgetPlaybackOutside(t->path());
        }
    });
    connect(t, &BrowserTab::selectionChanged, this, [this, t, current] {
        if (!current())
            return;
        updateStatus();
    });
    connect(t, &BrowserTab::currentItemChanged, this, [this, t, current](const QString &p) {
        if (!current())
            return;
        if (m_previewPane->isVisible())
            m_previewPane->setPath(p.isEmpty() ? t->path() : p);
        if (m_quickLook && m_quickLook->isOpen() && !p.isEmpty())
            m_quickLook->showPath(p);
    });
    connect(t, &BrowserTab::quickLookRequested, this, &MainWindow::toggleQuickLook);
    connect(t, &BrowserTab::modeChanged, this, [this, current] {
        if (current())
            updateNav();
    });
    connect(t, &BrowserTab::openInNewTabRequested, this, [this](const QString &p) { addTab(p, false); });
    connect(t, &BrowserTab::extractRequested, this, [this](const QStringList &archives) {
        Job j;
        j.type = Job::Extract;
        j.sources = archives;
        runJob(j);
    });
    connect(t, &BrowserTab::dropRequested, this, &MainWindow::handleDrop);
    connect(t, &BrowserTab::renameRequested, this, &MainWindow::rename);
    // ← at the list's left edge goes to the sidebar (→ there comes back).
    connect(t, &BrowserTab::leftEdgeReached, this, [this] {
        if (!m_sidebarPanel->isVisible())
            return;
        focusSidebar();
    });
    connect(t, &BrowserTab::contextMenuRequested, this, [this](const QPoint &pos, bool onItem) { showContextMenu(pos, onItem); });
}

void MainWindow::focusSidebar()
{
    // Preserve the sidebar cursor across pane changes; only initialize it on first entry.
    m_sidebar->setCurrentPath(tab()->path());
    const QList<QTreeWidgetItem *> sel = m_sidebar->selectedItems();
    QTreeWidgetItem *start = m_sidebar->currentItem();
    if (!start || !start->parent())
        start = sel.isEmpty() ? m_sidebar->topLevelItem(0) ? m_sidebar->topLevelItem(0)->child(0) : nullptr : sel.first();
    if (start)
        m_sidebar->setCurrentItem(start, 0, QItemSelectionModel::NoUpdate);
    m_sidebar->setFocus(Qt::TabFocusReason);
}

void MainWindow::onTabChanged()
{
    BrowserTab *t = tab();
    if (!t)
        return;
    {
        QSignalBlocker block(m_search);
        m_search->setText(QString()); // search is per tab; keep the field in sync
        t->setSearch(QString());
    }
    updateNav();
    updateStatus();
    if (m_previewPane->isVisible())
        m_previewPane->setPath(t->currentItemPath().isEmpty() ? t->path() : t->currentItemPath());
    terminalFollow(t->path());
    forgetPlaybackOutside(t->path());
}

// A video or sound goes on where it stopped only while the browser stays in its folder.
void MainWindow::forgetPlaybackOutside(const QString &folder)
{
    m_previewPane->forgetPlaybackOutside(folder);
    if (m_quickLook)
        m_quickLook->preview()->forgetPlaybackOutside(folder);
}

void MainWindow::askLine(const QString &placeholder, const QString &note, std::function<void(const QString &)> done)
{
    // A one-line prompt in a small box in the middle of the window: Enter runs it,
    // Esc or a click away closes. Qt::Popup suppresses IME composition on Windows; a tool window
    // keeps Korean IME input.
    auto *pop = new AiPromptPopup(this, Qt::Tool | Qt::FramelessWindowHint);
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->setAttribute(Qt::WA_TranslucentBackground);
    auto *outer = new QVBoxLayout(pop);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *frame = new QFrame(pop);
    frame->setObjectName(QStringLiteral("aiPopup"));
    outer->addWidget(frame);
    auto *fl = new QVBoxLayout(frame);
    fl->setContentsMargins(14, 10, 14, 10);
    fl->setSpacing(4);
    auto *row = new QHBoxLayout;
    auto *icon = new QLabel(frame);
    icon->setPixmap(Theme::icon(QStringLiteral("sparkle"), Theme::colors().accent, 20).pixmap(20, 20));
    auto *edit = new QLineEdit(frame);
    edit->setObjectName(QStringLiteral("aiPrompt"));
    edit->setAttribute(Qt::WA_InputMethodEnabled, true);
    edit->setInputMethodHints(Qt::ImhNone);
    edit->setPlaceholderText(placeholder);
    row->addWidget(icon);
    row->addWidget(edit, 1);
    fl->addLayout(row);
    auto *noteLabel = new QLabel(note, frame);
    noteLabel->setObjectName(QStringLiteral("secondary"));
    noteLabel->setTextFormat(Qt::PlainText);
    noteLabel->setWordWrap(true);
    fl->addWidget(noteLabel);
    connect(edit, &QLineEdit::returnPressed, this, [pop, edit, done] {
        const QString text = edit->text().trimmed();
        if (text.isEmpty())
            return;
        pop->close();
        done(text);
    });
    pop->resize(qMin(480, width() - 40), pop->sizeHint().height());
    // A small box in the middle of the window.
    const QPoint at = mapToGlobal(QPoint((width() - pop->width()) / 2, (height() - pop->height()) / 2));
    pop->move(at);
    pop->show();
    pop->raise();
    pop->activateWindow();
    edit->setFocus(Qt::OtherFocusReason);
}

void MainWindow::runSelectionCommand(const QVariantMap &command, const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    const QString label = command.value(QStringLiteral("label")).toString();
    QString shown = command.value(QStringLiteral("command")).toString();
    if (!shown.contains(QLatin1String("{prompt}")))
        return runCommandNow(command, paths, QString());
    askLine(Gifiles::tr("%1 (선택한 %2개 항목)").arg(label).arg(paths.size()),
            shown.replace(QLatin1String("{prompt}"), QStringLiteral("…")) +
                (command.value(QStringLiteral("terminal"), true).toBool() ? Gifiles::tr(" · 터미널에서 실행") : QString()),
            [this, command, paths](const QString &prompt) { runCommandNow(command, paths, prompt); });
}

void MainWindow::runCommandNow(const QVariantMap &command, const QStringList &paths, const QString &prompt)
{
    const QString label = command.value(QStringLiteral("label")).toString();
    const QString dir = tab()->path();
    const QString line = TerminalWidget::expandCommand(command.value(QStringLiteral("command")).toString(), paths, prompt, dir);
    const bool terminal = command.value(QStringLiteral("terminal"), true).toBool();
    Log::write("command", QStringLiteral("%1 on %2 item(s), %3").arg(label).arg(paths.size()).arg(terminal ? "terminal" : "quiet"));
    if (terminal)
        runInTerminal(line);
    else
        runQuietly(label, line, paths.isEmpty() ? dir : QFileInfo(paths.first()).absolutePath());
}

// A command with terminal = false runs in the shell without a terminal; the list shows what it
// made (the folder watcher), and only a failure is reported, with what the command printed.
void MainWindow::runQuietly(const QString &label, const QString &command, const QString &dir)
{
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(dir);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    QString shell = Settings::instance()->value(Settings::TermShell).toString().trimmed();
#ifdef Q_OS_WIN
    if (shell.isEmpty())
        shell = QStringLiteral("powershell.exe");
    // -EncodedCommand (UTF-16, base64): no command-line quoting to get wrong; stop at the first
    // error so a failure gives a non-zero exit code; UTF-8 output for the report.
    const QString script = QStringLiteral("$ErrorActionPreference = 'Stop'; [Console]::OutputEncoding = [Text.Encoding]::UTF8; ") + command;
    const QByteArray utf16(reinterpret_cast<const char *>(script.utf16()), script.size() * 2);
    proc->setProgram(shell);
    proc->setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-EncodedCommand"),
                        QString::fromLatin1(utf16.toBase64())});
#else
    if (shell.isEmpty())
        shell = qEnvironmentVariable("SHELL", QStringLiteral("/bin/sh"));
    proc->setProgram(shell);
    proc->setArguments({QStringLiteral("-l"), QStringLiteral("-c"), command}); // login: the user's PATH
#endif
    showMessage(Gifiles::tr("'%1' 실행 중…").arg(label), 60000);
    // What it creates in the folder (the new folder, the archive) gets selected afterwards.
    const QDir::Filters all = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
    const QStringList before = QDir(dir).entryList(all);
    auto report = [this, label, command](const QString &why, const QString &output) {
        showMessage(Gifiles::tr("'%1' 실패").arg(label), 5000);
        auto *box = new QMessageBox(QMessageBox::Warning, Gifiles::tr("'%1' 실패").arg(label), why, QMessageBox::Ok, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setInformativeText(output.right(1500));
        box->setDetailedText(command);
        box->open();
    };
    connect(proc, &QProcess::finished, this, [this, proc, label, report, dir, all, before](int code, QProcess::ExitStatus status) {
        const QString out = QString::fromUtf8(proc->readAll()).trimmed();
        proc->deleteLater();
        Log::write("command", QStringLiteral("%1 finished, exit %2").arg(label).arg(code));
        if (status == QProcess::NormalExit && code == 0) {
            showMessage(Gifiles::tr("'%1' 완료").arg(label), 3000);
            QStringList made;
            for (const QString &name : QDir(dir).entryList(all))
                if (!before.contains(name))
                    made << QDir(dir).filePath(name);
            if (!made.isEmpty() && QDir(tab()->path()) == QDir(dir))
                tab()->selectPaths(made);
        } else
            report(Gifiles::tr("명령이 오류로 끝났습니다 (종료 코드 %1).").arg(code), out);
    });
    connect(proc, &QProcess::errorOccurred, this, [proc, label, shell, report](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        Log::write("command", QStringLiteral("%1: cannot start %2").arg(label, shell));
        report(Gifiles::tr("셸을 시작하지 못했습니다: %1").arg(shell), QString());
        proc->deleteLater();
    });
    proc->start();
}

void MainWindow::runInTerminal(const QString &command)
{
    setTerminalVisible(true); // shows the run; starts the shell if needed
    TerminalWidget *t = term();
    if (t->isBusy()) // a program is running there: run beside it
        t = addTerminal(tab()->path());
    t->runCommand(command);
    // A closing popup hands focus back to the list afterwards; take it for the terminal.
    QTimer::singleShot(0, this, [this] {
        activateWindow();
        if (term() && term()->isVisible())
            term()->setFocus();
    });
}

// Gives the terminal panel the given height; the file views take the rest.
void MainWindow::setTerminalHeight(int h)
{
    const QList<int> s = m_vsplit->sizes();
    const int total = s.value(0) + s.value(1);
    m_vsplit->setSizes({total - h, h});
}

void MainWindow::setTerminalVisible(bool visible, bool keepHeight)
{
    const bool wasOpen = m_termOpen;
    m_termOpen = visible;
    m_terminalAct->setChecked(visible);
    m_terminalAct->setText(visible ? Gifiles::tr("터미널 접기") : Gifiles::tr("터미널 펼치기"));
    m_termToggle->setToolTip(m_terminalAct->text());
    for (auto &[button, glyph] : m_themed)
        if (button == m_termToggle)
            glyph = visible ? QStringLiteral("chevron-down-small") : QStringLiteral("chevron-up-small");
    m_termToggle->setIcon(Theme::icon(visible ? QStringLiteral("chevron-down-small") : QStringLiteral("chevron-up-small")));
    m_termAdd->setVisible(visible);
    if (!visible) {
        // Fold: only the bar is left, at the bottom edge. The shell keeps running and unfolding
        // resumes where it was.
        if (!wasOpen)
            return;
        if (const int h = m_vsplit->sizes().value(1); h >= 80)
            m_termHeight = h; // a drag down to the bar doesn't count
        m_termStack->hide();
        setTerminalHeight(kTermHeaderHeight);
        tab()->focusView();
        return;
    }
    if (m_termTabs->count() == 0)
        addTerminal(tab()->path());
    m_termStack->show();
    if (!keepHeight && (!wasOpen || m_vsplit->sizes().value(1) < 80)) {
        const int total = m_vsplit->sizes().value(0) + m_vsplit->sizes().value(1);
        setTerminalHeight(m_termHeight >= 80 ? qMin(m_termHeight, total - 120) : qMax(160, total / 3));
    }
    TerminalWidget *t = term();
    if (!t->isRunning())
        t->start(tab()->path());
    else
        terminalFollow(tab()->path());
    term()->setFocus();
}

TerminalWidget *MainWindow::term() const
{
    return static_cast<TerminalWidget *>(m_termStack->currentWidget());
}

bool MainWindow::terminalHasFocus() const
{
    const QWidget *f = QApplication::focusWidget();
    return f && m_termStack->isVisible() && m_termStack->isAncestorOf(f);
}

TerminalWidget *MainWindow::addTerminal(const QString &cwd)
{
    auto *t = new TerminalWidget(m_termStack);
    connect(t, &TerminalWidget::titleChanged, this, [this, t] { updateTerminalTab(t); });
    // The shell's own cd's move the browser too (the "terminal ↔ files" link); only the shown tab's.
    connect(t, &TerminalWidget::cwdChanged, this, [this, t](const QString &dir) {
        updateTerminalTab(t);
        if (t != term() || !Settings::instance()->flag(Settings::TermSyncBack))
            return;
        if (QFileInfo(dir).canonicalFilePath() != QFileInfo(tab()->path()).canonicalFilePath()) {
            Log::write("terminal", QStringLiteral("shell cd → list follows: %1").arg(dir));
            tab()->navigate(dir);
        }
    });
    connect(t, &TerminalWidget::newTabRequested, this, [this] { addTerminal(tab()->path())->setFocus(); });
    connect(t, &TerminalWidget::closeRequested, this, [this, t] { closeTerminal(m_termStack->indexOf(t)); });
    t->start(cwd);
    const int i = m_termTabs->currentIndex() + 1; // next to the current one, like Chrome
    m_termStack->insertWidget(i, t);
    m_termTabs->insertTab(i, t->tabTitle());
    m_termTabs->show();
    m_termTitle->hide();
    updateTerminalTab(t);
    m_termTabs->setCurrentIndex(i);
    m_termStack->setCurrentIndex(i); // also when it is the first tab (no currentChanged from -1 → 0 before insert)
    return t;
}

void MainWindow::updateTerminalTab(TerminalWidget *t)
{
    const int i = m_termStack->indexOf(t);
    if (i < 0)
        return;
    m_termTabs->setTabText(i, t->tabTitle());
    m_termTabs->setTabToolTip(i, t->title());
}

void MainWindow::closeTerminal(int index)
{
    QWidget *w = m_termStack->widget(index);
    if (!w)
        return;
    const bool hadFocus = terminalHasFocus();
    m_termStack->removeWidget(w);
    m_termTabs->removeTab(index); // its currentChanged picks the neighbour
    w->deleteLater();             // ends that shell
    if (m_termTabs->count() == 0) {
        m_termTabs->hide();
        m_termTitle->show();
        setTerminalVisible(false);
    } else if (hadFocus) {
        term()->setFocus();
    }
}

QString MainWindow::debugState() const
{
    BrowserTab *t = tab();
    if (!t)
        return {};
    static const char *modes[] = {"list", "gallery", "columns"};
    QWidget *f = QApplication::focusWidget();
    return QStringLiteral("path=%1 mode=%2 sel=%3 focus=%4 terminal=%5")
        .arg(t->path(), QString::fromLatin1(modes[t->mode()]))
        .arg(t->selectedPaths().size())
        .arg(f ? QString::fromLatin1(f->metaObject()->className()) : QStringLiteral("-"),
             m_termOpen ? QStringLiteral("open") : QStringLiteral("folded"));
}

void MainWindow::terminalFollow(const QString &path)
{
    TerminalWidget *t = term();
    if (!t || !Settings::instance()->flag(Settings::TermFollowFolder))
        return;
    if (t->isBusy() && !t->isAt(path)) {
        Log::write("terminal", QStringLiteral("busy → new terminal tab at %1").arg(path));
        addTerminal(path); // the running program keeps its tab
        return;
    }
    if (!t->isAt(path))
        Log::write("terminal", QStringLiteral("follow folder (cd now or when the line is empty): %1").arg(path));
    t->followFolder(path); // now, or once the half-typed line is run or cleared
}

void MainWindow::closeTab(int index)
{
    if (m_tabs->count() <= 1) {
        close();
        return;
    }
    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    w->deleteLater();
}

void MainWindow::updateTabTitle(BrowserTab *t)
{
    const int i = m_tabs->indexOf(t);
    if (i < 0)
        return;
    m_tabs->setTabText(i, util::displayName(t->path()));
    m_tabs->setTabToolTip(i, QDir::toNativeSeparators(t->path()));
    m_tabs->setTabIcon(i, QFileIconProvider().icon(QFileInfo(t->path())));
}

void MainWindow::updateNav()
{
    BrowserTab *t = tab();
    if (!t)
        return;
    setWindowTitle(util::displayName(t->path()));
    setWindowFilePath(t->path());
    m_pathBar->setPath(t->path());
    m_sidebar->setCurrentPath(t->path());
    m_back->setEnabled(t->canGoBack());
    m_forward->setEnabled(t->canGoForward());
    m_up->setEnabled(!QDir(t->path()).isRoot());
    (t->mode() == BrowserTab::Gallery ? m_modeGallery : t->mode() == BrowserTab::Columns ? m_modeColumns : m_modeList)->setChecked(true);
    m_iconSlider->setVisible(t->mode() == BrowserTab::Gallery);
    {
        QSignalBlocker block(m_iconSlider);
        m_iconSlider->setValue(t->iconSize());
    }
    m_biggerAct->setEnabled(t->mode() == BrowserTab::Gallery);
    m_smallerAct->setEnabled(t->mode() == BrowserTab::Gallery);
}

void MainWindow::updateStatus()
{
    BrowserTab *t = tab();
    if (!t || m_runningJobs > 0)
        return;
    const int n = t->itemCount();
    const int sel = t->selectedPaths().size();
    QString s = Gifiles::tr("%1개 항목").arg(n);
    if (sel > 0)
        s += Gifiles::tr(", %1개 선택됨").arg(sel);
    const QStorageInfo si(t->path());
    if (si.isValid())
        s += Gifiles::tr(", %1 사용 가능").arg(util::humanSize(si.bytesAvailable()));
    m_statusLabel->setText(s);
}

void MainWindow::updateUndoActions()
{
    App *app = App::instance();
    m_undo->setEnabled(app->canUndo());
    m_redo->setEnabled(app->canRedo());
    m_undo->setText(app->canUndo() ? Gifiles::tr("%1 실행 취소").arg(app->undoLabel()) : Gifiles::tr("실행 취소"));
    m_redo->setText(app->canRedo() ? Gifiles::tr("%1 실행 복귀").arg(app->redoLabel()) : Gifiles::tr("실행 복귀"));
}

void MainWindow::go(const QString &path)
{
    tab()->navigate(path);
}

void MainWindow::setMode(BrowserTab::Mode m)
{
    tab()->setMode(m, true);
    updateNav();
    updateStatus();
}

// ---------------------------------------------------------------------------
// File operations

void MainWindow::newFolder()
{
    QString created;
    UndoRecord rec;
    if (QString err = FileOps::makeFolder(tab()->path(), Gifiles::tr("무제 폴더"), &created, &rec); !err.isEmpty()) {
        QMessageBox::warning(this, QString(), err);
        return;
    }
    App::instance()->recordDone(rec);
    tab()->selectPaths({created}, true);
}

void MainWindow::newFolderWithSelection()
{
    const QStringList sel = tab()->selectedPaths();
    if (sel.isEmpty())
        return;
    // The new folder goes next to the items (they may be inside an expanded subfolder).
    const QString dir = QFileInfo(sel.first()).absolutePath();
    QString created;
    UndoRecord rec;
    const QString err = FileOps::makeFolderWith(dir, sel, &created, &rec);
    App::instance()->recordDone(rec);
    if (!err.isEmpty())
        QMessageBox::warning(this, QString(), err);
    if (!created.isEmpty())
        tab()->selectPaths({created}, true);
}

void MainWindow::rename(const QString &path, const QString &newName)
{
    UndoRecord rec;
    if (QString err = FileOps::rename(path, newName, &rec); !err.isEmpty()) {
        QMessageBox::warning(this, QString(), err);
        return;
    }
    App::instance()->recordDone(rec);
    if (!rec.isEmpty())
        tab()->selectPaths({rec.steps.first().to});
}

void MainWindow::duplicate()
{
    const QStringList sel = tab()->selectedPaths();
    if (sel.isEmpty())
        return;
    Job j;
    j.type = Job::Duplicate;
    j.sources = sel;
    runJob(j);
}

void MainWindow::moveToTrash()
{
    const QStringList sel = tab()->selectedPaths();
    if (sel.isEmpty())
        return;
    Job j;
    j.type = Job::Trash;
    j.sources = sel;
    runJob(j, tab()->neighborOfSelection());
}

void MainWindow::copyToClipboard(bool cut)
{
    const QStringList sel = tab()->selectedPaths();
    if (sel.isEmpty())
        return;
    auto *md = new QMimeData;
    QList<QUrl> urls;
    for (const QString &p : sel)
        urls << QUrl::fromLocalFile(p);
    md->setUrls(urls);
    QStringList native;
    for (const QString &p : sel)
        native << QDir::toNativeSeparators(p);
    md->setText(native.join(QLatin1Char('\n')));
    if (cut) {
        md->setData(kCutMime, "1");
        md->setData(kWinDropEffect, QByteArray("\x02\x00\x00\x00", 4)); // Explorer understands this as "cut"
    }
    QGuiApplication::clipboard()->setMimeData(md);
    App::instance()->setCutPaths(cut ? sel : QStringList());
    showMessage(Gifiles::tr("%1개 항목 %2").arg(sel.size()).arg(cut ? Gifiles::tr("잘라냄") : Gifiles::tr("복사함")), 3000);
}

void MainWindow::paste(bool forceMove)
{
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md || !md->hasUrls())
        return;
    const QStringList srcs = localPaths(md->urls());
    if (srcs.isEmpty())
        return;
    const QByteArray effect = md->data(kWinDropEffect);
    const bool cut = forceMove || md->hasFormat(kCutMime) || (!effect.isEmpty() && (effect.at(0) & 2));
    runTransfer(srcs, tab()->path(), cut);
    if (cut) {
        QGuiApplication::clipboard()->clear();
        App::instance()->setCutPaths({});
    }
}

void MainWindow::copyPath()
{
    const QStringList sel = tab()->selectedPaths();
    QStringList out;
    for (const QString &p : sel.isEmpty() ? QStringList{tab()->path()} : sel)
        out << QDir::toNativeSeparators(p);
    QGuiApplication::clipboard()->setText(out.join(QLatin1Char('\n')));
    showMessage(Gifiles::tr("경로를 복사함"), 2000);
}

void MainWindow::undo()
{
    if (m_runningJobs > 0) {
        m_deferredUndo << true;
        return;
    }
    if (!App::instance()->canUndo())
        return;
    Job j;
    j.type = Job::Undo;
    j.record = App::instance()->takeUndo();
    runJob(j);
}

void MainWindow::redo()
{
    if (m_runningJobs > 0) {
        m_deferredUndo << false;
        return;
    }
    if (!App::instance()->canRedo())
        return;
    Job j;
    j.type = Job::Redo;
    j.record = App::instance()->takeRedo();
    runJob(j);
}

void MainWindow::handleDrop(const QList<QUrl> &urls, const QString &targetDir, Qt::DropAction proposed)
{
    const QStringList srcs = localPaths(urls);
    if (srcs.isEmpty() || targetDir.isEmpty())
        return;
    // Finder rules: same volume moves, another volume copies; ⌥ forces copy, ⌘ forces move.
    // (On Windows/Linux: Ctrl copies, Shift moves.)
    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
    bool move;
#ifdef Q_OS_MACOS
    if (mods & Qt::AltModifier)
        move = false;
    else if (mods & Qt::ControlModifier)
        move = true;
#else
    if (mods & Qt::ControlModifier)
        move = false;
    else if (mods & Qt::ShiftModifier)
        move = true;
#endif
    else {
        const QString targetVol = QStorageInfo(targetDir).rootPath();
        move = true;
        for (const QString &s : srcs)
            if (QStorageInfo(s).rootPath() != targetVol)
                move = false;
        if (proposed == Qt::CopyAction && !move)
            move = false;
    }
    runTransfer(srcs, targetDir, move);
}

bool MainWindow::resolveConflicts(const QStringList &sources, const QString &destDir, QHash<QString, Conflict> *out)
{
    QStringList clashing;
    for (const QString &s : sources) {
        const QFileInfo fi(s);
        if (QDir::cleanPath(fi.absolutePath()) != QDir::cleanPath(destDir) && util::exists(QDir(destDir).filePath(fi.fileName())))
            clashing << s;
    }
    bool applyAll = false;
    Conflict all = Conflict::KeepBoth;
    for (int i = 0; i < clashing.size(); ++i) {
        const QString &s = clashing[i];
        if (applyAll) {
            out->insert(s, all);
            continue;
        }
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setText(Gifiles::tr("이 위치에 \"%1\"(이)라는 이름의 항목이 이미 있습니다.").arg(QFileInfo(s).fileName()));
        box.setInformativeText(Gifiles::tr("대치하면 기존 항목은 휴지통으로 이동합니다."));
        QPushButton *stop = box.addButton(Gifiles::tr("중단"), QMessageBox::RejectRole);
        QPushButton *skip = box.addButton(Gifiles::tr("건너뛰기"), QMessageBox::NoRole);
        QPushButton *keep = box.addButton(Gifiles::tr("둘 다 유지"), QMessageBox::AcceptRole);
        QPushButton *replace = box.addButton(Gifiles::tr("대치"), QMessageBox::DestructiveRole);
        box.setDefaultButton(keep);
        QCheckBox *cb = nullptr;
        if (clashing.size() - i > 1) {
            cb = new QCheckBox(Gifiles::tr("나머지 %1개 항목에도 적용").arg(clashing.size() - i - 1));
            box.setCheckBox(cb);
        }
        box.exec();
        Conflict c;
        if (box.clickedButton() == stop || !box.clickedButton())
            return false;
        if (box.clickedButton() == skip)
            c = Conflict::Skip;
        else if (box.clickedButton() == replace)
            c = Conflict::Replace;
        else
            c = Conflict::KeepBoth;
        Q_UNUSED(keep);
        out->insert(s, c);
        if (cb && cb->isChecked()) {
            applyAll = true;
            all = c;
        }
    }
    return true;
}

void MainWindow::runTransfer(const QStringList &sources, const QString &destDir, bool move)
{
    Job j;
    j.type = move ? Job::Move : Job::Copy;
    j.sources = sources;
    j.destDir = destDir;
    if (!resolveConflicts(sources, destDir, &j.conflicts))
        return;
    runJob(j);
}

void MainWindow::runJob(const Job &job, const QString &selectAfter)
{
    auto *watcher = new QFutureWatcher<OpResult>(this);
    const QString verb = FileOps::verb(job.type);
    Log::write("job", QStringLiteral("start %1: %2 → %3").arg(verb, job.sources.join(QStringLiteral(", ")), job.destDir));
    ++m_runningJobs;
    m_progress->setValue(0);
    m_progress->show();
    m_cancel->show();
    m_statusLabel->setText(Gifiles::tr("%1 중…").arg(verb));
    const auto cancel = job.cancel;
    m_cancel->disconnect();
    connect(m_cancel, &QToolButton::clicked, this, [cancel] { cancel->store(true); });
    connect(watcher, &QFutureWatcher<OpResult>::progressRangeChanged, m_progress, &QProgressBar::setRange);
    connect(watcher, &QFutureWatcher<OpResult>::progressValueChanged, m_progress, &QProgressBar::setValue);
    connect(watcher, &QFutureWatcher<OpResult>::progressTextChanged, this, [this, verb](const QString &t) {
        m_statusLabel->setText(Gifiles::tr("%1 중… %2").arg(verb, t));
    });
    connect(watcher, &QFutureWatcher<OpResult>::finished, this, [this, watcher, selectAfter] {
        watcher->deleteLater();
        if (--m_runningJobs == 0) {
            m_progress->hide();
            m_cancel->hide();
            if (!m_deferredUndo.isEmpty()) // after this job's record is in (queued past this handler)
                QTimer::singleShot(0, this, [this] {
                    if (m_runningJobs == 0 && !m_deferredUndo.isEmpty())
                        m_deferredUndo.takeFirst() ? undo() : redo();
                });
        }
        if (watcher->future().resultCount() == 0)
            return;
        const OpResult r = watcher->result();
        Log::write("job", QStringLiteral("done %1: created %2, errors %3%4%5")
                              .arg(FileOps::verb(r.type))
                              .arg(r.created.size())
                              .arg(r.errors.size())
                              .arg(r.canceled ? QStringLiteral(", canceled") : QString(),
                                   r.errors.isEmpty() ? QString() : QStringLiteral(" | ") + r.errors.join(QStringLiteral(" | "))));
        App *app = App::instance();
        switch (r.type) {
        case Job::Undo: // a canceled undo leaves the steps it didn't reach on the undo stack
            app->recordUndone(r.record);
            app->recordRedone(r.rest);
            break;
        case Job::Redo:
            app->recordRedone(r.record);
            app->recordUndone(r.rest);
            break;
        default: app->recordDone(r.record); break;
        }
        updateStatus();
        if (!r.created.isEmpty()) {
            const QString dir = QFileInfo(r.created.first()).absolutePath();
            QStringList here;
            for (const QString &c : r.created)
                if (QFileInfo(c).absolutePath() == dir)
                    here << c;
            if (tab()->path() == dir || (tab()->mode() == BrowserTab::Columns && util::isInside(tab()->path(), dir)))
                tab()->selectPaths(here);
        } else if (!selectAfter.isEmpty()) {
            tab()->selectPaths({selectAfter});
        }
        if (r.canceled)
            showMessage(Gifiles::tr("%1 중단됨").arg(FileOps::verb(r.type)), 4000);
        if (!r.errors.isEmpty()) {
            qWarning().noquote() << "file operation errors:" << r.errors;
            // Window-modal, not exec(): a nested event loop here would stall whatever runs after the job.
            auto *box = new QMessageBox(QMessageBox::Warning, QString(), Gifiles::tr("일부 항목을 처리하지 못했습니다."),
                                        QMessageBox::Ok, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setDetailedText(r.errors.join(QLatin1Char('\n')));
            box->setInformativeText(r.errors.first());
            box->open();
        }
    });
    watcher->setFuture(FileOps::start(job));
}

// ---------------------------------------------------------------------------
// Quick Look, info, context menu

void MainWindow::toggleQuickLook()
{
    if (m_quickLook && m_quickLook->isVisible()) {
        m_quickLook->close();
        return;
    }
    if (m_quickLook && m_quickLook->isOpen()) { // pressed again before it appeared
        m_quickLook->cancelPresent();
        return;
    }
    const QString p = tab()->currentItemPath();
    if (p.isEmpty())
        return;
    if (!m_quickLook) {
        m_quickLook = new QuickLookWindow(this);
        connect(m_quickLook, &QuickLookWindow::closed, this, [this] {
            activateWindow();
            if (m_quickLookTarget)
                m_quickLookTarget->setFocus();
        });
    }
    QWidget *fw = QApplication::focusWidget();
    m_quickLookTarget = qobject_cast<QAbstractItemView *>(fw) ? fw : tab()->view();
    m_quickLook->setForwardTarget(m_quickLookTarget);
    m_quickLook->placeAround(geometry().center());
    m_quickLook->showPath(p);
    m_quickLook->present();
}

namespace {
qint64 dirSize(const QString &path, int *count)
{
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
        ++*count;
    }
    return total;
}
} // namespace

void MainWindow::revealInFileManager()
{
    QStringList paths = tab()->selectedPaths();
    if (paths.isEmpty())
        paths << tab()->path(); // the empty area's menu: the current folder
    for (const QString &p : paths.mid(0, 8)) {
        const util::Command c = util::revealCommand(p);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        QProcess::startDetached(c.program, c.args);
#else
        // No FileManager1 service (or no dbus-send): open the folder with xdg-open.
        auto *proc = new QProcess(this);
        const QFileInfo fi(p);
        const QString folder = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
        connect(proc, &QProcess::finished, this, [proc, folder](int code, QProcess::ExitStatus st) {
            if (code != 0 || st != QProcess::NormalExit)
                QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
            proc->deleteLater();
        });
        connect(proc, &QProcess::errorOccurred, this, [proc, folder](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
                proc->deleteLater();
            }
        });
        proc->start(c.program, c.args);
#endif
    }
}

void MainWindow::showInfo()
{
    QStringList paths = tab()->selectedPaths();
    if (paths.isEmpty())
        paths << tab()->path();
    for (const QString &p : paths.mid(0, 8)) {
        const QFileInfo fi(p);
        auto *dlg = new QDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(Gifiles::tr("%1 정보").arg(util::displayName(p)));
        auto *form = new QFormLayout(dlg);
        auto *icon = new QLabel(dlg);
        icon->setPixmap(QFileIconProvider().icon(fi).pixmap(64, 64));
        auto *name = new QLabel(util::displayName(p), dlg);
        QFont f = name->font();
        f.setBold(true);
        name->setFont(f);
        form->addRow(icon, name);
        form->addRow(Gifiles::tr("종류:"), new QLabel(util::kindOf(fi), dlg));
        auto *size = new QLabel(fi.isDir() ? Gifiles::tr("계산 중…") : Gifiles::tr("%1 (%2바이트)").arg(util::humanSize(fi.size()), QLocale().toString(fi.size())), dlg);
        form->addRow(Gifiles::tr("크기:"), size);
        auto *where = new QLabel(QDir::toNativeSeparators(fi.absolutePath()), dlg);
        where->setTextInteractionFlags(Qt::TextSelectableByMouse);
        where->setWordWrap(true);
        form->addRow(Gifiles::tr("위치:"), where);
        form->addRow(Gifiles::tr("생성일:"), new QLabel(QLocale().toString(fi.birthTime(), QLocale::LongFormat), dlg));
        form->addRow(Gifiles::tr("수정일:"), new QLabel(QLocale().toString(fi.lastModified(), QLocale::LongFormat), dlg));
        if (fi.isSymLink())
            form->addRow(Gifiles::tr("원본:"), new QLabel(QDir::toNativeSeparators(fi.symLinkTarget()), dlg));
        const QFile::Permissions pm = fi.permissions();
        form->addRow(Gifiles::tr("권한:"), new QLabel(QStringLiteral("%1%2%3")
                                                         .arg(pm & QFile::ReadUser ? Gifiles::tr("읽기 ") : QString())
                                                         .arg(pm & QFile::WriteUser ? Gifiles::tr("쓰기 ") : QString())
                                                         .arg(pm & QFile::ExeUser ? Gifiles::tr("실행") : QString()),
                                                     dlg));
        if (fi.isDir()) {
            auto *w = new QFutureWatcher<QPair<qint64, int>>(dlg);
            connect(w, &QFutureWatcher<QPair<qint64, int>>::finished, size, [w, size] {
                const auto r = w->result();
                size->setText(Gifiles::tr("%1 (%2개 파일)").arg(util::humanSize(r.first)).arg(r.second));
            });
            w->setFuture(QtConcurrent::run([p] {
                int n = 0;
                qint64 s = dirSize(p, &n);
                return qMakePair(s, n);
            }));
        }
        dlg->setMinimumWidth(360);
        dlg->move(pos() + QPoint(60 + 24 * paths.indexOf(p), 60 + 24 * paths.indexOf(p)));
        dlg->show();
    }
}

// On macOS the menu bar's keys reach this window's actions while another of its windows (Settings,
// Get Info, Quick Look, a message) is in front: ⌘W closes that window, not a tab or this window.
bool MainWindow::closeFrontWindow()
{
    QWidget *front = QApplication::activeWindow();
    if (!front || front == this || qobject_cast<MainWindow *>(front))
        return false;
    front->close();
    return true;
}

namespace {

// Context-menu letters: each item shows one after its name, "(O)", and pressing it runs the item
// (a submenu opens with its first item chosen). Fixed letters following the usual Windows
// conventions (Open O, Cut T, Copy C, Paste P, Delete D, Rename M, Copy as path A, …), not the
// user's shortcuts. A key on the Korean 2-set layout gives a jamo; it counts as the key's letter.
QChar menuLetter(const QKeyEvent *e)
{
    if ((e->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier)) != Qt::NoModifier)
        return {};
    const int k = e->key();
    if ((k >= Qt::Key_A && k <= Qt::Key_Z) || (k >= Qt::Key_0 && k <= Qt::Key_9))
        return QChar(k);
    static const QString jamo = QStringLiteral("ㅂㅈㄷㄱㅅㅛㅕㅑㅐㅔㅁㄴㅇㄹㅎㅗㅓㅏㅣㅋㅌㅊㅍㅠㅜㅡㅃㅉㄸㄲㅆㅒㅖ");
    static const QString keys = QStringLiteral("QWERTYUIOPASDFGHJKLZXCVBNMQWERTOP");
    const QString t = e->text();
    if (t.size() == 1)
        if (const qsizetype i = jamo.indexOf(t[0]); i >= 0)
            return keys[i];
    return {};
}

class MenuLetters : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        auto *menu = qobject_cast<QMenu *>(obj);
        if (!menu || (ev->type() != QEvent::KeyPress && ev->type() != QEvent::ShortcutOverride))
            return false;
        const QChar c = menuLetter(static_cast<QKeyEvent *>(ev));
        for (QAction *a : menu->actions()) {
            if (a->isSeparator() || !a->isEnabled() || !a->isVisible())
                continue;
            const QString id = a->property("menuShortcutId").toString();
            auto *key = static_cast<QKeyEvent *>(ev);
            QKeyEvent normalized(key->type(), c.isNull() ? key->key() : c.unicode(),
                                 key->modifiers() & ~Qt::KeypadModifier, key->text());
            if (id.isEmpty() ? (c.isNull() || a->property("menuLetter").toChar() != c)
                             : !Shortcuts::instance()->matches(id, &normalized))
                continue;
            if (ev->type() == QEvent::ShortcutOverride) {
                ev->accept();
                return true;
            }
            menu->setActiveAction(a);
            if (a->menu()) { // → opens it with its first item chosen
                QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
                QCoreApplication::sendEvent(menu, &right);
                return true;
            }
            // Close the whole menu (submenus included), then run the item: the same on every
            // platform, unlike a synthetic Return.
            while (QWidget *popup = QApplication::activePopupWidget())
                popup->close();
            a->trigger();
            return true;
        }
        return false;
    }
};

} // namespace

void MainWindow::showContextMenu(const QPoint &globalPos, bool onItem, bool fromKey)
{
    Theme::ShortcutMenu menu(this);
    auto *letters = new MenuLetters(&menu);
    menu.installEventFilter(letters);
    // The window's own actions get their letter for as long as the menu is open.
    QList<QPair<QPointer<QAction>, QString>> renamed;
    auto letter = [&renamed](QAction *a, char c, bool shared = true) {
        a->setProperty("menuLetter", QChar(QLatin1Char(c)));
        if (shared)
            renamed.append({a, a->text()});
        static const QHash<QString, QString> names = {
            {QStringLiteral("menuOpen"), QStringLiteral("열기")}, {QStringLiteral("다음으로 열기…"), QStringLiteral("다음으로 열기")},
            {QStringLiteral("새로운 탭에서 열기"), QStringLiteral("새로운 탭에서 열기")}, {QStringLiteral("퀵 뷰어"), QStringLiteral("퀵 뷰어")},
            {util::revealActionId(), QStringLiteral("외부 파일관리자")}, {QStringLiteral("이름 변경"), QStringLiteral("이름 변경")},
            {QStringLiteral("복제"), QStringLiteral("복제")}, {QStringLiteral("선택한 항목들로…"), QStringLiteral("선택 항목 명령")},
            {QStringLiteral("선택 명령 편집…"), QStringLiteral("명령 편집")}, {QStringLiteral("경로 복사"), QStringLiteral("경로 복사")},
            {QStringLiteral("잘라내기"), QStringLiteral("잘라내기")}, {QStringLiteral("복사하기"), QStringLiteral("복사")},
            {QStringLiteral("휴지통으로 이동"), QStringLiteral("휴지통")}, {QStringLiteral("새로운 폴더"), QStringLiteral("새 폴더")},
            {QStringLiteral("붙여넣기"), QStringLiteral("붙여넣기")}, {QStringLiteral("항목을 여기로 이동"), QStringLiteral("이동")},
            {QStringLiteral("갤러리로"), QStringLiteral("갤러리")}, {QStringLiteral("목록으로"), QStringLiteral("목록")},
            {QStringLiteral("컬럼으로"), QStringLiteral("컬럼")}, {QStringLiteral("숨김 파일 보기"), QStringLiteral("숨김 파일")}
        };
        QString shown = QString(QLatin1Char(c));
        if (names.contains(a->objectName())) {
            const QString id = QStringLiteral("컨텍스트 메뉴 ") + names.value(a->objectName());
            a->setProperty("menuShortcutId", id);
            const auto keys = Shortcuts::instance()->keys(id);
            shown = keys.isEmpty() ? QString() : keys.first().toString(QKeySequence::NativeText);
        }
        if (!shown.isEmpty())
            a->setText(a->text() + QStringLiteral(" (%1)").arg(shown));
    };
    auto add = [&menu, &letter](QAction *a, char c) {
        menu.addAction(a);
        letter(a, c);
    };
    if (onItem) {
        bool anyFile = false;
        for (const QString &p : tab()->selectedPaths())
            anyFile = anyFile || !QFileInfo(p).isDir();
        // Not m_openAct: on files that one shows this menu.
        const int count = tab()->selectedPaths().size();
        QAction *open = menu.addAction(m_openAct->icon(), count > 1 ? Gifiles::tr("열기 (%1개 항목)").arg(count) : Gifiles::tr("열기"));
        open->setObjectName(QStringLiteral("menuOpen"));
        letter(open, 'O', false);
        connect(open, &QAction::triggered, this, [this] { tab()->openSelection(false); });
        if (fromKey)
            menu.setActiveAction(open);
        if (anyFile)
            add(m_openWithAct, 'H');
        if (tab()->hasSelectedFolder())
            add(m_openTabAct, 'E');
        add(m_quickLookAct, 'Q');
        menu.addSeparator();
        add(m_revealAct, 'I');
        add(m_renameAct, 'M');
        add(m_duplicateAct, 'U');
        // "선택한 항목들로…": the user's commands (config.toml [selection_menu], Settings → 선택 항목 메뉴),
        // with the selection filled in; each has its own letter (key).
        QMenu *with = menu.addMenu(Theme::icon(QStringLiteral("folder-plus"), {}, 16), Gifiles::tr("선택한 항목들로…"));
        with->menuAction()->setObjectName(QStringLiteral("선택한 항목들로…"));
        with->installEventFilter(letters);
        letter(with->menuAction(), 'S', false);
        const QVariantList commands = Settings::instance()->value(Settings::SelectionCommands).toList();
        for (const QVariant &c : commands) {
            const QVariantMap m = c.toMap();
            const QString label = m.value(QStringLiteral("label")).toString(), cmd = m.value(QStringLiteral("command")).toString();
            QAction *a = with->addAction(label + (cmd.contains(QLatin1String("{prompt}")) ? QStringLiteral("…") : QString()), this,
                                         [this, m] { runSelectionCommand(m, tab()->selectedPaths()); });
            if (const QString k = m.value(QStringLiteral("key")).toString().toUpper(); k.size() == 1)
                letter(a, k[0].toLatin1(), false);
        }
        if (!commands.isEmpty())
            with->addSeparator();
        auto *edit = with->addAction(Gifiles::tr("메뉴 편집…"), this, [this] { SettingsDialog::showSingleton(this, Gifiles::tr("선택 항목 메뉴")); });
        edit->setObjectName(QStringLiteral("선택 명령 편집…"));
        letter(edit, 'E', false);
        add(m_copyPathAct, 'A');
        menu.addSeparator();
        add(m_cutAct, 'T');
        add(m_copyAct, 'C');
        menu.addSeparator();
        add(m_trashAct, 'D');
    } else {
        add(m_newFolderAct, 'F');
        add(m_pasteAct, 'P');
        add(m_moveHereAct, 'M');
        menu.addSeparator();
        add(m_revealAct, 'I');
        add(m_copyPathAct, 'A');
        menu.addSeparator();
        add(m_modeGallery, '1'); // as ⌘1 / ⌘2 / ⌘3
        add(m_modeList, '2');
        add(m_modeColumns, '3');
        menu.addSeparator();
        add(m_showHidden, 'H');
    }
    // Names go back before a chosen item runs (it may log or show its own name).
    auto restore = [&renamed] {
        for (const auto &[a, text] : std::as_const(renamed))
            if (a)
                a->setText(text);
        renamed.clear();
    };
    connect(&menu, &QMenu::aboutToHide, this, restore);
    if (const QString dir = qEnvironmentVariable("GIFILES_SNAPSHOT"); !dir.isEmpty()) {
        // Wayland requires real input before a grabbing popup; render the native menu directly.
        menu.ensurePolished();
        menu.resize(menu.sizeHint());
        menu.grab().save(QDir(dir).filePath(QStringLiteral("context-menu.png")));
        restore();
        return;
    }
    menu.exec(globalPos);
    restore();
}

// ---------------------------------------------------------------------------
// Session

QVariantMap MainWindow::saveState() const
{
    QStringList paths;
    QVariantList modes;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *t = static_cast<BrowserTab *>(m_tabs->widget(i));
        paths << t->path();
        modes << int(t->mode());
    }
    return {{QStringLiteral("tabs"), paths},
            {QStringLiteral("modes"), modes},
            {QStringLiteral("current"), m_tabs->currentIndex()},
            {QStringLiteral("geometry"), saveGeometry()},
            {QStringLiteral("splitter"), m_splitter->saveState()},
            {QStringLiteral("sidebar"), m_sidebarPanel->isVisible()},
            {QStringLiteral("preview"), m_previewPane->isVisible()},
            {QStringLiteral("terminal"), m_terminalAct->isChecked()},
            {QStringLiteral("terminalHeight"), m_terminalAct->isChecked() ? m_vsplit->sizes().value(1) : m_termHeight},
            {QStringLiteral("vsplit"), m_vsplit->saveState()}};
}

void MainWindow::restoreState(const QVariantMap &s)
{
    const QVariantList modes = s.value(QStringLiteral("modes")).toList();
    for (int i = 0; i < m_tabs->count() && i < modes.size(); ++i)
        static_cast<BrowserTab *>(m_tabs->widget(i))->setMode(BrowserTab::Mode(modes[i].toInt()));
    m_tabs->setCurrentIndex(qBound(0, s.value(QStringLiteral("current")).toInt(), m_tabs->count() - 1));
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    m_splitter->restoreState(s.value(QStringLiteral("splitter")).toByteArray());
    const bool sidebar = s.value(QStringLiteral("sidebar"), true).toBool();
    const bool preview = s.value(QStringLiteral("preview"), false).toBool();
    m_showSidebar->setChecked(sidebar);
    m_sidebarPanel->setVisible(sidebar);
    updateToolbarInset();
    m_showPreview->setChecked(preview);
    m_previewPane->setVisible(preview);
    m_vsplit->restoreState(s.value(QStringLiteral("vsplit")).toByteArray());
    m_termHeight = s.value(QStringLiteral("terminalHeight"), 0).toInt();
    if (!s.value(QStringLiteral("terminal"), false).toBool())
        setTerminalHeight(kTermHeaderHeight);
    else
        QTimer::singleShot(0, this, [this] { setTerminalVisible(true); });
    updateNav();
    updateStatus();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (m_quickLook) {
        m_quickLook->cancelPresent();
        m_quickLook->close();
    }
    App::instance()->windowClosing(this);
    QMainWindow::closeEvent(e);
}

void MainWindow::checkForUpdates()
{
    Updater *u = Updater::instance();
    const auto offerRestart = [this, u] {
        if (QMessageBox::question(this, Gifiles::tr("업데이트"),
                                  Gifiles::tr("Gifiles %1을(를) 받아 두었습니다 (지금 %2). 지금 다시 시작해 설치할까요?\n"
                                              "나중에 앱을 끌 때도 설치됩니다.").arg(u->readyVersion(), u->version())) == QMessageBox::Yes)
            restartToUpdate();
    };
    if (u->state() == Updater::State::Off) {
        QMessageBox::information(this, Gifiles::tr("업데이트"), Gifiles::tr("Gifiles %1\n\n%2").arg(u->version(), u->offReason()));
        return;
    }
    auto *wait = new QObject(this); // lives until this check has an answer
    connect(u, &Updater::stateChanged, wait, [this, u, wait, offerRestart] {
        switch (u->state()) {
        case Updater::State::Checking:
        case Updater::State::Downloading:
            return;
        case Updater::State::Ready:
            wait->deleteLater();
            offerRestart();
            return;
        case Updater::State::Failed:
            wait->deleteLater();
            QMessageBox::warning(this, Gifiles::tr("업데이트"), u->error());
            return;
        default:
            wait->deleteLater();
            QMessageBox::information(this, Gifiles::tr("업데이트"),
                                     Gifiles::tr("최신 버전입니다 (Gifiles %1).").arg(u->version()));
        }
    });
    u->check();
}

void MainWindow::restartToUpdate()
{
    if (Updater::instance()->apply(true))
        App::instance()->quit();
    else
        QMessageBox::warning(this, Gifiles::tr("업데이트"), Gifiles::tr("업데이트를 설치하지 못했습니다. 앱을 끌 때 다시 시도합니다."));
}
