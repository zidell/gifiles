#include "Settings.h"
#include "Util.h"
#include "App.h"
#include "RecentFolders.h"
#include "BrowserTab.h"
#include "MainWindow.h"

#include <QApplication>
#include <QMessageBox>
#include <QPointer>
#include <QClipboard>
#include <QMimeData>
#include <QDir>
#include <QScreen>
#include <QSettings>

App *App::instance()
{
    static App *a = new App(qApp);
    return a;
}

App::App(QObject *parent) : QObject(parent)
{
    // Anything else put on the clipboard (a copy, another app) cancels a pending cut.
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        const QMimeData *md = QGuiApplication::clipboard()->mimeData();
        if (!md || !md->hasFormat(QStringLiteral("application/x-gifiles-cut")))
            setCutPaths({});
    });
    m_showHidden = Settings::instance()->flag(Settings::ShowHidden);
    connect(Settings::instance(), &Settings::problemsFound, this, &App::showConfigProblems);
    connect(Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String(Settings::ShowHidden))
            setShowHidden(Settings::instance()->flag(Settings::ShowHidden));
    });
}

MainWindow *App::newWindow(const QStringList &tabPaths, QWidget *near)
{
    auto *w = new MainWindow(tabPaths.isEmpty() ? QStringList{QDir::homePath()} : tabPaths);
    m_windows << w;
    if (near) {
        w->resize(near->size());
        w->move(near->pos() + QPoint(28, 28));
    }
    w->show();
    w->raise();
    w->activateWindow();
    return w;
}

void App::restoreSession()
{
    QSettings s;
    const QVariantList wins = s.value(QStringLiteral("session/windows")).toList();
    for (const QVariant &v : wins) {
        QVariantMap m = v.toMap();
        // Tabs whose folder is gone are left out; their views and the current tab follow the rest.
        const QStringList tabs = m.value(QStringLiteral("tabs")).toStringList();
        const QVariantList modes = m.value(QStringLiteral("modes")).toList();
        const int current = m.value(QStringLiteral("current")).toInt();
        QStringList paths;
        QVariantList keptModes;
        int keptCurrent = -1; // the current tab, or the next one left (as when a tab is closed)
        for (int i = 0; i < tabs.size(); ++i) {
            if (!QFileInfo(tabs[i]).isDir())
                continue;
            if (i >= current && keptCurrent < 0)
                keptCurrent = int(paths.size());
            paths << tabs[i];
            if (i < modes.size())
                keptModes << modes[i];
        }
        if (paths.isEmpty())
            continue;
        m.insert(QStringLiteral("modes"), keptModes);
        m.insert(QStringLiteral("current"), keptCurrent < 0 ? int(paths.size()) - 1 : keptCurrent);
        MainWindow *w = newWindow(paths);
        w->restoreState(m);
    }
    if (m_windows.isEmpty())
        newWindow();
}

void App::saveSession(const QList<MainWindow *> &windows)
{
    QVariantList out;
    for (MainWindow *w : windows)
        out << w->saveState();
    QSettings().setValue(QStringLiteral("session/windows"), out);
}

void App::windowClosing(MainWindow *w)
{
    m_windows.removeAll(w);
    m_windows.removeAll(nullptr);
    if (m_windows.isEmpty() && !m_quitting)
        saveSession({w});
}

void App::quit()
{
    QList<MainWindow *> live;
    for (const auto &w : m_windows)
        if (w)
            live << w;
    saveSession(live);
    m_quitting = true;
    qApp->closeAllWindows();
    qApp->quit();
}

void App::setCutPaths(const QStringList &paths)
{
    const QSet<QString> next(paths.begin(), paths.end());
    if (next == m_cut)
        return;
    m_cut = next;
    emit cutChanged();
}

void App::showConfigProblems(const QStringList &problems)
{
    static QPointer<QMessageBox> box;
    if (box)
        box->close();
    QWidget *parent = QApplication::activeWindow();
    if (!parent && !m_windows.isEmpty())
        parent = m_windows.constLast().data();
    box = new QMessageBox(QMessageBox::Warning, Gifiles::tr("설정 파일 확인"),
                          Gifiles::tr("config.toml에서 쓸 수 없는 값을 찾았습니다. 그 항목은 이전 값을 그대로 씁니다."),
                          QMessageBox::Ok, parent);
    box->setInformativeText(QStringLiteral("• ") + problems.join(QStringLiteral("\n• ")) + QStringLiteral("\n\n") +
                            QDir::toNativeSeparators(Settings::configPath()));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

void App::setShowHidden(bool show)
{
    if (show == m_showHidden)
        return;
    m_showHidden = show;
    Settings::instance()->setValue(Settings::ShowHidden, show);
    emit showHiddenChanged(show);
}

void App::recordDone(const UndoRecord &rec)
{
    if (rec.isEmpty())
        return;
    RecentFolders::instance()->noteRecord(rec); // where something was just done
    m_undo << rec;
    if (m_undo.size() > 100)
        m_undo.removeFirst();
    m_redo.clear();
    emit undoChanged();
}

void App::recordUndone(const UndoRecord &rec)
{
    if (!rec.isEmpty())
        m_redo << rec;
    emit undoChanged();
}

void App::recordRedone(const UndoRecord &rec)
{
    if (!rec.isEmpty())
        m_undo << rec;
    emit undoChanged();
}

UndoRecord App::takeUndo()
{
    UndoRecord r = m_undo.takeLast();
    emit undoChanged();
    return r;
}

UndoRecord App::takeRedo()
{
    UndoRecord r = m_redo.takeLast();
    emit undoChanged();
    return r;
}
