#include "Settings.h"
#include "Util.h"
#include "OpenWith.h"
#include "RecentFolders.h"
#include "Theme.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMimeDatabase>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace OpenWith {

namespace {
QString prefKey(const QString &file)
{
    return QStringLiteral("open_with/") + QFileInfo(file).suffix().toLower();
}
} // namespace

QList<App> appsFor(const QString &file)
{
    QList<App> apps = platformApps(file);
    // The remembered app goes first even if the system doesn't list it.
    const QString remembered = Settings::instance()->value(prefKey(file)).toString();
    if (!remembered.isEmpty()) {
        auto it = std::find_if(apps.begin(), apps.end(), [&](const App &a) { return a.id == remembered; });
        if (it == apps.end() && QFileInfo::exists(remembered))
            apps.prepend(App{QFileInfo(remembered).completeBaseName(), remembered,
                             QFileIconProvider().icon(QFileInfo(remembered)), false});
    }
    return apps;
}

bool openWith(const QStringList &files, const QString &appId)
{
    if (!platformOpen(files, appId))
        return false;
    for (const QString &f : files) // opening a file to work on it makes its folder a recent one
        RecentFolders::instance()->note(QFileInfo(f).absolutePath());
    return true;
}

void open(const QString &file)
{
    RecentFolders::instance()->note(QFileInfo(file).absolutePath());
    const QString remembered = Settings::instance()->value(prefKey(file)).toString();
    if (!remembered.isEmpty() && openWith({file}, remembered))
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(file));
}

bool hasRememberedApp(const QString &file)
{
    return !Settings::instance()->value(prefKey(file)).toString().isEmpty();
}

void showDialog(QWidget *parent, const QStringList &files)
{
    if (files.isEmpty())
        return;
#ifdef Q_OS_WIN
    Q_UNUSED(parent);
    // Windows' own picker, including "Always use this app to open .ext files".
    QProcess::startDetached(QStringLiteral("rundll32.exe"),
                            {QStringLiteral("shell32.dll,OpenAs_RunDLL"), QDir::toNativeSeparators(files.first())});
#else
    const QString file = files.first();
    const QString ext = QFileInfo(file).suffix().toLower();
    auto *dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(Gifiles::tr("다음으로 열기"));
    dlg->resize(420, 460);
    auto *root = new QVBoxLayout(dlg);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(10);
    auto *title = new QLabel(files.size() == 1 ? Gifiles::tr("\"%1\"을(를) 열 앱을 선택하세요").arg(QFileInfo(file).fileName())
                                               : Gifiles::tr("%1개 항목을 열 앱을 선택하세요").arg(files.size()),
                             dlg);
    title->setObjectName(QStringLiteral("title"));
    title->setWordWrap(true);
    root->addWidget(title);

    auto *list = new QListWidget(dlg);
    list->setObjectName(QStringLiteral("settingsNav"));
    list->setIconSize(QSize(28, 28));
    const QString remembered = Settings::instance()->value(prefKey(file)).toString();
    for (const App &a : appsFor(file)) {
        auto *it = new QListWidgetItem(a.icon, a.isDefault ? Gifiles::tr("%1  (기본)").arg(a.name) : a.name, list);
        it->setData(Qt::UserRole, a.id);
        it->setSizeHint(QSize(0, 40));
        if (a.id == remembered || (remembered.isEmpty() && a.isDefault))
            list->setCurrentItem(it);
    }
    if (!list->currentItem() && list->count())
        list->setCurrentRow(0);
    root->addWidget(list, 1);

    auto *other = new QPushButton(Gifiles::tr("다른 앱 선택…"), dlg);
    root->addWidget(other, 0, Qt::AlignLeft);
    QCheckBox *always = nullptr;
    if (!ext.isEmpty()) {
        always = new QCheckBox(Gifiles::tr("항상 이 앱으로 .%1 파일 열기").arg(ext), dlg);
        always->setObjectName(QStringLiteral("switch"));
        always->setChecked(!remembered.isEmpty());
        root->addWidget(always);
    }
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *cancel = new QPushButton(Gifiles::tr("취소"), dlg);
    auto *ok = new QPushButton(Gifiles::tr("열기"), dlg);
    ok->setObjectName(QStringLiteral("primary"));
    ok->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(ok);
    root->addLayout(buttons);

    auto accept = [dlg, list, always, files, file] {
        QListWidgetItem *it = list->currentItem();
        if (!it)
            return;
        const QString id = it->data(Qt::UserRole).toString();
        if (always && always->isChecked())
            Settings::instance()->setValue(prefKey(file), id);
        else if (always)
            Settings::instance()->remove(prefKey(file));
        openWith(files, id);
        dlg->accept();
    };
    QObject::connect(ok, &QPushButton::clicked, dlg, accept);
    QObject::connect(list, &QListWidget::itemDoubleClicked, dlg, accept);
    QObject::connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);
    QObject::connect(other, &QPushButton::clicked, dlg, [dlg, list] {
#ifdef Q_OS_MACOS
        const QString app = QFileDialog::getOpenFileName(dlg, Gifiles::tr("앱 선택"), QStringLiteral("/Applications"),
                                                         Gifiles::tr("응용 프로그램 (*.app)"));
#else
        const QString app = QFileDialog::getOpenFileName(dlg, Gifiles::tr("앱 선택"), QStringLiteral("/usr/share/applications"),
                                                         Gifiles::tr("응용 프로그램 (*.desktop)"));
#endif
        if (app.isEmpty())
            return;
        auto *it = new QListWidgetItem(QFileIconProvider().icon(QFileInfo(app)), QFileInfo(app).completeBaseName(), list);
        it->setData(Qt::UserRole, app);
        it->setSizeHint(QSize(0, 40));
        list->setCurrentItem(it);
    });
    dlg->open();
#endif
}

#ifndef Q_OS_MACOS
#ifdef Q_OS_WIN
QList<App> platformApps(const QString &)
{
    return {};
}

bool platformOpen(const QStringList &files, const QString &appId)
{
    bool ok = true;
    for (const QString &f : files)
        ok = QProcess::startDetached(appId, {QDir::toNativeSeparators(f)}) && ok;
    return ok;
}
#else
// Linux: registered apps for the MIME type come from `gio mime`; names and icons from .desktop files.
QList<App> platformApps(const QString &file)
{
    const QString mime = QMimeDatabase().mimeTypeForFile(file).name();
    QProcess gio;
    gio.start(QStringLiteral("gio"), {QStringLiteral("mime"), mime});
    if (!gio.waitForFinished(2000))
        return {};
    const QStringList lines = QString::fromUtf8(gio.readAllStandardOutput()).split(QLatin1Char('\n'));
    QString defaultId;
    QStringList ids;
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        if (t.startsWith(QLatin1String("Default application")))
            defaultId = t.section(QLatin1Char(':'), 1).trimmed();
        else if (t.endsWith(QLatin1String(".desktop")) && !ids.contains(t))
            ids << t;
    }
    if (!defaultId.isEmpty() && !ids.contains(defaultId))
        ids.prepend(defaultId);
    QList<App> apps;
    for (const QString &id : ids) {
        const QString path = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, id);
        QSettings desktop(path, QSettings::IniFormat);
        desktop.beginGroup(QStringLiteral("Desktop Entry"));
        const QString name = desktop.value(QStringLiteral("Name"), id).toString();
        const QIcon icon = QIcon::fromTheme(desktop.value(QStringLiteral("Icon")).toString());
        apps << App{name, id, icon, id == defaultId};
    }
    return apps;
}

bool platformOpen(const QStringList &files, const QString &appId)
{
    QStringList args{appId};
    for (const QString &f : files)
        args << f;
    return QProcess::startDetached(QStringLiteral("gtk-launch"), args);
}
#endif
#endif

} // namespace OpenWith
