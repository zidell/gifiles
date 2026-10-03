#include "Settings.h"
#include "Util.h"
#include "Permissions.h"
#include "Theme.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

#ifdef Q_OS_MACOS
QString bundlePath()
{
    QDir d(QCoreApplication::applicationDirPath()); // .../Gifiles.app/Contents/MacOS
    d.cdUp();
    d.cdUp();
    return d.absolutePath();
}
#endif
} // namespace

namespace Permissions {

bool hasFullDiskAccess()
{
#ifdef Q_OS_MACOS
    // The per-user TCC database is readable only with Full Disk Access. Probing it never shows
    // a prompt: without access the open simply fails.
    QFile f(QDir::homePath() + QStringLiteral("/Library/Application Support/com.apple.TCC/TCC.db"));
    if (f.open(QIODevice::ReadOnly))
        return true;
    return !f.exists() && QFile(QStringLiteral("/Library/Application Support/com.apple.TCC/TCC.db")).open(QIODevice::ReadOnly);
#else
    return true;
#endif
}

void showDialog(QWidget *parent, bool force)
{
#ifdef Q_OS_MACOS
    if (hasFullDiskAccess()) {
        if (force)
            QMessageBox::information(parent, QString(), Gifiles::tr("전체 디스크 접근 권한이 이미 허용되어 있습니다."));
        return;
    }
    if (!force && Settings::instance()->flag(Settings::DontAskFullDisk))
        return;

    auto *dlg = new QDialog(parent, Qt::Sheet);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setFixedWidth(440);
    auto *root = new QVBoxLayout(dlg);
    root->setContentsMargins(28, 26, 28, 22);
    root->setSpacing(12);

    auto *icon = new QLabel(dlg);
    icon->setPixmap(Theme::icon(QStringLiteral("shield"), Theme::colors().accent, 52).pixmap(52, 52));
    icon->setAlignment(Qt::AlignCenter);
    root->addWidget(icon);

    auto *title = new QLabel(Gifiles::tr("폴더 접근을 한 번에 허용해 주세요"), dlg);
    title->setObjectName(QStringLiteral("title"));
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    auto *body = new QLabel(Gifiles::tr("macOS는 데스크탑, 문서, 다운로드, 외장 디스크를 처음 열 때마다 따로 허용을 묻습니다. "
        "<b>전체 디스크 접근 권한</b>을 켜 두면 다시 묻지 않습니다."), dlg);
    body->setObjectName(QStringLiteral("secondary"));
    body->setWordWrap(true);
    body->setAlignment(Qt::AlignCenter);
    root->addWidget(body);

    auto *steps = new QLabel(Gifiles::tr("<ol style='margin-left:-20px'>"
        "<li><b>설정 열기</b>를 누르면 시스템 설정이 열립니다.</li>"
        "<li>목록에서 <b>Gifiles</b>를 켭니다.</li>"
        "<li>목록에 없으면 함께 열린 Finder 창의 Gifiles를 목록으로 끌어다 놓습니다.</li>"
        "</ol>"), dlg);
    steps->setWordWrap(true);
    root->addWidget(steps);

    auto *status = new QLabel(dlg);
    status->setAlignment(Qt::AlignCenter);
    status->setObjectName(QStringLiteral("secondary"));
    status->hide();
    root->addWidget(status);

    QCheckBox *dontAsk = nullptr;
    if (!force) {
        dontAsk = new QCheckBox(Gifiles::tr("다시 묻지 않기"), dlg);
        root->addWidget(dontAsk);
    }

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *later = new QPushButton(Gifiles::tr("나중에"), dlg);
    auto *open = new QPushButton(Gifiles::tr("설정 열기"), dlg);
    open->setObjectName(QStringLiteral("primary"));
    open->setDefault(true);
    buttons->addWidget(later);
    buttons->addWidget(open);
    root->addSpacing(4);
    root->addLayout(buttons);

    QObject::connect(later, &QPushButton::clicked, dlg, [dlg, dontAsk] {
        if (dontAsk && dontAsk->isChecked())
            Settings::instance()->setValue(Settings::DontAskFullDisk, true);
        dlg->close();
    });

    // Poll while the user is in System Settings; close by ourselves once access is granted.
    auto *poll = new QTimer(dlg);
    poll->setInterval(1000);
    QObject::connect(poll, &QTimer::timeout, dlg, [dlg, poll, status, open] {
        if (!hasFullDiskAccess())
            return;
        poll->stop();
        status->setText(Gifiles::tr("허용되었습니다. 이제 폴더마다 묻지 않습니다."));
        open->setEnabled(false);
        QTimer::singleShot(1400, dlg, &QDialog::close);
    });
    QObject::connect(open, &QPushButton::clicked, dlg, [poll, status, open] {
        // Reveal the app first so the Settings window ends up in front, with Gifiles ready to drag.
        QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), bundlePath()});
        QTimer::singleShot(400, [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral(
                "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")));
        });
        status->setText(Gifiles::tr("시스템 설정에서 Gifiles를 켜면 자동으로 확인합니다…"));
        status->show();
        open->setText(Gifiles::tr("설정 다시 열기"));
        poll->start();
    });
    dlg->open();
#else
    if (force)
        QMessageBox::information(parent, QString(), Gifiles::tr("이 운영체제에서는 따로 허용할 필요가 없습니다."));
#endif
}

} // namespace Permissions
