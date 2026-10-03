#include "Updater.h"

#include "Settings.h"
#include "Util.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QVersionNumber>
#include <QtConcurrent/QtConcurrentRun>

#include <cstdlib>
#include <memory>

extern "C" {
#include "tweetnacl.h"
// TweetNaCl wants a random source for key generation; only verification is used here.
void randombytes(unsigned char *, unsigned long long) { std::abort(); }
}

namespace {

const char *kRepo = "https://github.com/zidell/gifiles/releases/";

// The public half of the key the release job signs update.json with (the private half is the
// UPDATE_SIGNING_KEY secret of the repository).
const unsigned char kPublicKey[32] = {0x03, 0x46, 0x31, 0x56, 0xdf, 0x4d, 0x80, 0x48, 0x41, 0xcc, 0xe8,
                                      0x40, 0xb8, 0x97, 0x6b, 0xd1, 0x77, 0x50, 0xf4, 0xce, 0xe5, 0x1f,
                                      0x4a, 0x22, 0x0c, 0x7a, 0xa7, 0xb2, 0x5d, 0x35, 0x8f, 0x39};

const int kMaxManifest = 64 * 1024;

bool g_allowed = false; // Updater::allowUpdates()

QString platformKey()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("windows-x64");
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("linux-x86_64");
#else
    return {};
#endif
}

// What an update replaces, or "" when this copy can't be updated in place.
QString installTarget()
{
#if defined(Q_OS_MACOS)
    QDir d(QCoreApplication::applicationDirPath()); // Gifiles.app/Contents/MacOS
    d.cdUp();
    d.cdUp();
    return d.absolutePath().endsWith(QLatin1String(".app")) ? d.absolutePath() : QString();
#elif defined(Q_OS_WIN)
    return QCoreApplication::applicationDirPath();
#else
    return qEnvironmentVariable("APPIMAGE"); // set by the AppImage runtime
#endif
}

Updater::Config defaultConfig()
{
    Updater::Config c;
    c.manifestBase = QUrl(QString::fromLatin1(kRepo) + QStringLiteral("latest/download/"));
    c.assetBase = QUrl(QString::fromLatin1(kRepo) + QStringLiteral("download/"));
    c.publicKey = QByteArray(reinterpret_cast<const char *>(kPublicKey), sizeof kPublicKey);
    c.version = QCoreApplication::applicationVersion();
    c.platform = platformKey();
    c.target = installTarget();
    c.staging = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/update");
    c.teamId = QStringLiteral("AF68GKBM82");
    if (!g_allowed)
        c.offReason = Gifiles::tr("직접 빌드한 앱은 자동으로 업데이트하지 않습니다. GitHub 릴리스의 배포판만 업데이트합니다.");
    else if (c.platform.isEmpty())
        c.offReason = Gifiles::tr("이 플랫폼용 배포판이 없어 자동으로 업데이트하지 않습니다.");
    else if (c.target.isEmpty())
#ifdef Q_OS_LINUX
        c.offReason = Gifiles::tr("AppImage로 실행할 때만 자동으로 업데이트합니다.");
#else
        c.offReason = Gifiles::tr("앱이 있는 위치를 알 수 없어 자동으로 업데이트하지 않습니다.");
#endif
    else if (!QFileInfo(QFileInfo(c.target).absolutePath()).isWritable())
        c.offReason = Gifiles::tr("앱이 있는 폴더(%1)에 쓸 수 없어 자동으로 업데이트하지 않습니다.")
                          .arg(QDir::toNativeSeparators(QFileInfo(c.target).absolutePath()));
    return c;
}

// Runs a tool to the end; false (with its first error line) when it fails.
bool run(const QString &program, const QStringList &args, QString *err)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted() || !p.waitForFinished(5 * 60 * 1000) || p.exitStatus() != QProcess::NormalExit ||
        p.exitCode() != 0) {
        const QString msg = QString::fromLocal8Bit(p.readAllStandardError()).trimmed().section(QLatin1Char('\n'), 0, 0);
        *err = msg.isEmpty() ? Gifiles::tr("%1을(를) 실행할 수 없습니다.").arg(QFileInfo(program).fileName()) : msg;
        return false;
    }
    return true;
}

QByteArray sha256Of(const QString &path)
{
    QFile f(path);
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!f.open(QIODevice::ReadOnly) || !h.addData(&f))
        return {};
    return h.result();
}

// Unpacks a downloaded, hash-checked file into what replaces the app; returns its path or "".
QString stage(const Updater::Config &c, const QString &file, QString *err)
{
    const QString out = QFileInfo(file).absolutePath() + QStringLiteral("/unpacked");
    QDir(out).removeRecursively();
    QDir().mkpath(out);
    QString staged;
    if (c.platform == QLatin1String("macos")) {
        if (!run(QStringLiteral("/usr/bin/ditto"), {QStringLiteral("-x"), QStringLiteral("-k"), file, out}, err))
            return {};
        staged = out + QStringLiteral("/Gifiles.app");
        if (!QFileInfo(staged).isDir()) {
            *err = Gifiles::tr("받은 파일에 Gifiles.app이 없습니다.");
            return {};
        }
        if (!c.teamId.isEmpty()) {
            const QString req = QStringLiteral("=identifier \"dev.zidell.gifiles\" and anchor apple generic and "
                                               "certificate leaf[subject.OU] = \"%1\"").arg(c.teamId);
            if (!run(QStringLiteral("/usr/bin/codesign"),
                     {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"),
                      QStringLiteral("-R"), req, staged}, err)) {
                *err = Gifiles::tr("받은 앱의 서명이 맞지 않습니다: %1").arg(*err);
                return {};
            }
        }
    } else if (c.platform.startsWith(QLatin1String("windows"))) {
        if (!run(QStringLiteral("tar.exe"), {QStringLiteral("-xf"), file, QStringLiteral("-C"), out}, err))
            return {};
        staged = out + QStringLiteral("/Gifiles");
        if (!QFileInfo::exists(staged + QStringLiteral("/Gifiles.exe"))) {
            *err = Gifiles::tr("받은 파일에 Gifiles.exe가 없습니다.");
            return {};
        }
    } else {
        staged = out + QStringLiteral("/Gifiles.AppImage");
        if (!QFile::rename(file, staged)) {
            *err = Gifiles::tr("받은 파일을 옮기지 못했습니다.");
            return {};
        }
        QFile::setPermissions(staged, QFile::permissions(staged) | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther |
                                          QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther);
    }
    return staged;
}

} // namespace

void Updater::allowUpdates() { g_allowed = true; }

Updater *Updater::instance()
{
    static Updater *u = [] {
        auto *x = new Updater(defaultConfig(), qApp);
        x->setAutomatic(Settings::instance()->flag(Settings::AutoUpdate));
        connect(Settings::instance(), &Settings::changed, x, [x](const QString &key) {
            if (key.isEmpty() || key == QLatin1String(Settings::AutoUpdate))
                x->setAutomatic(Settings::instance()->flag(Settings::AutoUpdate));
        });
        return x;
    }();
    return u;
}

Updater::Updater(const Config &config, QObject *parent) : QObject(parent), m_config(config)
{
    if (!m_config.offReason.isEmpty())
        m_state = State::Off;
}

void Updater::setState(State s, const QString &error)
{
    m_state = s;
    m_error = error;
    emit stateChanged();
}

void Updater::setAutomatic(bool on)
{
    if (m_state == State::Off)
        return;
    if (!on) {
        delete m_timer;
        m_timer = nullptr;
        return;
    }
    if (m_timer)
        return;
    m_timer = new QTimer(this);
    m_timer->setInterval(6 * 60 * 60 * 1000);
    connect(m_timer, &QTimer::timeout, this, &Updater::check);
    m_timer->start();
    QTimer::singleShot(60 * 1000, this, [this] {
        if (m_timer)
            check();
    });
}

void Updater::check()
{
    if (m_state == State::Off || m_state == State::Checking || m_state == State::Downloading || m_applied)
        return;
    if (!m_net) {
        m_net = new QNetworkAccessManager(this);
        m_net->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy); // GitHub redirects to its file host
        m_net->setTransferTimeout(60 * 1000);
    }
    setState(State::Checking);
    auto got = std::make_shared<QPair<QByteArray, QByteArray>>();
    auto pending = std::make_shared<int>(2);
    auto failed = std::make_shared<QString>();
    const auto fetch = [&](const QString &name, bool manifest) {
        QNetworkReply *r = m_net->get(QNetworkRequest(m_config.manifestBase.resolved(QUrl(name))));
        connect(r, &QNetworkReply::finished, this, [this, r, manifest, got, pending, failed] {
            r->deleteLater();
            const QByteArray body = r->read(kMaxManifest + 1);
            if (r->error() != QNetworkReply::NoError)
                *failed = r->errorString();
            else if (body.size() > kMaxManifest)
                *failed = Gifiles::tr("업데이트 정보가 너무 큽니다.");
            else
                (manifest ? got->first : got->second) = body;
            if (--*pending == 0) {
                if (!failed->isEmpty())
                    setState(State::Failed, Gifiles::tr("새 버전을 확인하지 못했습니다: %1").arg(*failed));
                else
                    fetched(got->first, got->second);
            }
        });
    };
    fetch(QStringLiteral("update.json"), true);
    fetch(QStringLiteral("update.json.sig"), false);
}

void Updater::fetched(const QByteArray &manifest, const QByteArray &signature)
{
    if (!verifySignature(manifest, QByteArray::fromBase64(signature.trimmed()), m_config.publicKey))
        return setState(State::Failed, Gifiles::tr("업데이트 정보의 서명이 맞지 않아 무시했습니다."));
    const Manifest m = parseManifest(manifest, m_config.platform);
    if (!m.error.isEmpty())
        return setState(State::Failed, m.error);
    m_latest = m.version;
    if (!isNewer(m.version, m_config.version) || !m.hasAsset)
        return setState(m_readyVersion.isEmpty() ? State::Idle : State::Ready);
    if (m.version == m_readyVersion)
        return setState(State::Ready); // already downloaded and unpacked
    download(m);
}

void Updater::download(const Manifest &m)
{
    setState(State::Downloading);
    const QString dir = m_config.staging + QLatin1Char('/') + m.version;
    m_readyVersion.clear(); // a newer version replaces one that was waiting
    m_staged.clear();
    QDir(m_config.staging).removeRecursively(); // older downloads
    QDir().mkpath(dir);
    auto file = std::make_shared<QFile>(dir + QLatin1Char('/') + m.file);
    if (!file->open(QIODevice::WriteOnly))
        return setState(State::Failed, Gifiles::tr("업데이트를 받을 폴더에 쓸 수 없습니다: %1").arg(QDir::toNativeSeparators(dir)));
    QNetworkRequest req(m_config.assetBase.resolved(QUrl(QStringLiteral("v%1/%2").arg(m.version, m.file))));
    QNetworkReply *r = m_net->get(req);
    connect(r, &QNetworkReply::readyRead, this, [r, file, m] {
        file->write(r->readAll());
        if (file->size() > m.size)
            r->abort();
    });
    connect(r, &QNetworkReply::finished, this, [this, r, file, m] {
        r->deleteLater();
        file->write(r->readAll());
        file->close();
        if (r->error() != QNetworkReply::NoError)
            return setState(State::Failed, Gifiles::tr("새 버전을 받지 못했습니다: %1").arg(r->errorString()));
        unpack(m, file->fileName());
    });
}

void Updater::unpack(const Manifest &m, const QString &file)
{
    auto *w = new QFutureWatcher<QPair<QString, QString>>(this);
    connect(w, &QFutureWatcherBase::finished, this, [this, w, m] {
        w->deleteLater();
        const auto [staged, err] = w->result();
        if (staged.isEmpty())
            return setState(State::Failed, err);
        m_staged = staged;
        m_readyVersion = m.version;
        setState(State::Ready);
    });
    const Config c = m_config;
    w->setFuture(QtConcurrent::run([c, m, file]() -> QPair<QString, QString> {
        if (QFileInfo(file).size() != m.size || sha256Of(file) != m.sha256) {
            QFile::remove(file);
            return {QString(), Gifiles::tr("받은 파일이 업데이트 정보와 다릅니다 (크기나 SHA-256). 무시했습니다.")};
        }
        QString err;
        const QString staged = stage(c, file, &err);
        QFile::remove(file);
        return {staged, err};
    }));
}

bool Updater::apply(bool relaunch)
{
    if (m_state != State::Ready || m_applied || m_staged.isEmpty())
        return false;
    const QString script = writeHelper(m_config.staging);
    if (script.isEmpty())
        return false;
    const auto [program, args] = helperCommand(script, QCoreApplication::applicationPid(), m_staged, m_config.target, relaunch);
    if (!QProcess::startDetached(program, args, m_config.staging))
        return false;
    m_applied = true;
    return true;
}

bool Updater::verifySignature(const QByteArray &message, const QByteArray &signature, const QByteArray &publicKey)
{
    if (signature.size() != crypto_sign_BYTES || publicKey.size() != crypto_sign_PUBLICKEYBYTES)
        return false;
    const QByteArray signedMessage = signature + message;
    QByteArray out(signedMessage.size(), Qt::Uninitialized);
    unsigned long long outLen = 0;
    return crypto_sign_open(reinterpret_cast<unsigned char *>(out.data()), &outLen,
                            reinterpret_cast<const unsigned char *>(signedMessage.constData()),
                            static_cast<unsigned long long>(signedMessage.size()),
                            reinterpret_cast<const unsigned char *>(publicKey.constData())) == 0;
}

Updater::Manifest Updater::parseManifest(const QByteArray &json, const QString &platform)
{
    Manifest m;
    QJsonParseError pe;
    const QJsonObject o = QJsonDocument::fromJson(json, &pe).object();
    static const QRegularExpression versionRe(QStringLiteral("^\\d+(\\.\\d+){1,3}$"));
    static const QRegularExpression fileRe(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    m.version = o.value(QStringLiteral("version")).toString();
    if (pe.error != QJsonParseError::NoError || !versionRe.match(m.version).hasMatch()) {
        m.error = Gifiles::tr("업데이트 정보를 읽을 수 없습니다.");
        return m;
    }
    const QJsonValue a = o.value(QStringLiteral("assets")).toObject().value(platform);
    if (platform.isEmpty() || !a.isObject())
        return m; // no file for this platform in that release
    const QJsonObject asset = a.toObject();
    m.file = asset.value(QStringLiteral("file")).toString();
    m.sha256 = QByteArray::fromHex(asset.value(QStringLiteral("sha256")).toString().toLatin1());
    m.size = asset.value(QStringLiteral("size")).toInteger();
    if (!fileRe.match(m.file).hasMatch() || m.file.contains(QLatin1String("..")) || m.sha256.size() != 32 || m.size <= 0) {
        m.error = Gifiles::tr("업데이트 정보를 읽을 수 없습니다.");
        return m;
    }
    m.hasAsset = true;
    return m;
}

bool Updater::isNewer(const QString &candidate, const QString &current)
{
    const QVersionNumber a = QVersionNumber::fromString(candidate), b = QVersionNumber::fromString(current);
    return !a.isNull() && QVersionNumber::compare(a, b.isNull() ? QVersionNumber(0) : b) > 0;
}

QString Updater::writeHelper(const QString &dir)
{
    QDir().mkpath(dir);
#ifdef Q_OS_WIN
    const QString path = dir + QStringLiteral("/apply-update.ps1");
    const char *text = R"PS(# Gifiles update: waits for the app to exit, then puts the new version in its place.
param([int]$ProcessId, [string]$New, [string]$Target, [int]$Relaunch)
$ErrorActionPreference = 'Stop'
for ($i = 0; $i -lt 600 -and (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 200 }
if (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) { exit 1 }
$name = Split-Path -Leaf $Target
$tmp = "$Target.new"; $old = "$Target.old"
foreach ($p in @($tmp, $old)) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force } }
Copy-Item -LiteralPath $New -Destination $tmp -Recurse
# Files can stay locked for a moment after the process ends (virus scanners): retry.
function Retry($block) { for ($i = 0; ; $i++) { try { & $block; return } catch { if ($i -ge 20) { throw }; Start-Sleep -Milliseconds 250 } } }
Retry { Rename-Item -LiteralPath $Target -NewName (Split-Path -Leaf $old) }
try { Retry { Rename-Item -LiteralPath $tmp -NewName $name } }
catch { Rename-Item -LiteralPath $old -NewName $name; Remove-Item -LiteralPath $tmp -Recurse -Force; exit 1 }
Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $New -Recurse -Force -ErrorAction SilentlyContinue
if ($Relaunch -eq 1) { Start-Process -FilePath (Join-Path $Target 'Gifiles.exe') }
exit 0
)PS";
#else
    const QString path = dir + QStringLiteral("/apply-update.sh");
    const char *text = R"SH(#!/bin/sh
# Gifiles update: waits for the app to exit, then puts the new version in its place.
pid="$1"; new="$2"; target="$3"; relaunch="$4"
i=0
while kill -0 "$pid" 2>/dev/null; do
    i=$((i + 1)); [ "$i" -gt 600 ] && exit 1
    sleep 0.2
done
tmp="$target.new"; old="$target.old"
rm -rf "$tmp" "$old"
if command -v ditto >/dev/null 2>&1; then ditto "$new" "$tmp"; else cp -R "$new" "$tmp"; fi || { rm -rf "$tmp"; exit 1; }
[ -f "$tmp" ] && chmod 755 "$tmp"
mv "$target" "$old" || { rm -rf "$tmp"; exit 1; }
if ! mv "$tmp" "$target"; then mv "$old" "$target"; rm -rf "$tmp"; exit 1; fi
rm -rf "$old" "$new"
if [ "$relaunch" = 1 ]; then
    if [ -d "$target" ] && command -v open >/dev/null 2>&1; then open "$target"
    else nohup "$target" >/dev/null 2>&1 & fi
fi
exit 0
)SH";
#endif
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text) < 0)
        return {};
    f.close();
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

QPair<QString, QStringList> Updater::helperCommand(const QString &script, qint64 pid, const QString &staged,
                                                   const QString &target, bool relaunch)
{
    const QString r = relaunch ? QStringLiteral("1") : QStringLiteral("0");
#ifdef Q_OS_WIN
    return {QStringLiteral("powershell.exe"),
            {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"),
             QStringLiteral("Bypass"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"), QStringLiteral("-File"),
             QDir::toNativeSeparators(script), QStringLiteral("-ProcessId"), QString::number(pid), QStringLiteral("-New"),
             QDir::toNativeSeparators(staged), QStringLiteral("-Target"), QDir::toNativeSeparators(target),
             QStringLiteral("-Relaunch"), r}};
#else
    return {QStringLiteral("/bin/sh"), {script, QString::number(pid), staged, target, r}};
#endif
}
