// Automatic updates (src/Updater): the signed update information, downloading and checking a
// release from a local folder (file:// URLs), unpacking it per platform, and the helper that swaps
// the new version in after the app has quit.
//
//   ./build/gifiles_unit_update

#include "Headless.h"

#include "Updater.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

extern "C" {
#include "tweetnacl.h"
}

namespace {

// A key made for these tests only (openssl genpkey -algorithm ed25519): seed and public key.
const QByteArray kSeed = QByteArray::fromHex("a4b031fb4e3f6ca416091aa0ad11329a399ecb33a4370874ebf9ec2355af4d19");
const QByteArray kPublic = QByteArray::fromHex("95fd89029f2f9abd3a5b8ae1c2f526396f95b38368a93a5960d362403d3e768c");

// Ed25519 signing is deterministic, so TweetNaCl needs no random source for it.
QByteArray sign(const QByteArray &message)
{
    const QByteArray sk = kSeed + kPublic;
    QByteArray sm(message.size() + crypto_sign_BYTES, Qt::Uninitialized);
    unsigned long long smLen = 0;
    crypto_sign(reinterpret_cast<unsigned char *>(sm.data()), &smLen, reinterpret_cast<const unsigned char *>(message.constData()),
                message.size(), reinterpret_cast<const unsigned char *>(sk.constData()));
    return sm.left(crypto_sign_BYTES);
}

QString platform()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows-x64");
#else
    return QStringLiteral("linux-x86_64");
#endif
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool runTool(const QString &program, const QStringList &args, const QString &dir = {})
{
    QProcess p;
    if (!dir.isEmpty())
        p.setWorkingDirectory(dir);
    p.start(program, args);
    return p.waitForFinished(60000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// The app as it is installed on this platform, holding `marker`: Gifiles.app, a folder with
// Gifiles.exe, or the AppImage file.
QString makeApp(const QString &dir, const QString &name, const QByteArray &marker)
{
#if defined(Q_OS_MACOS)
    const QString app = dir + QLatin1Char('/') + name + QStringLiteral(".app");
    writeFile(app + QStringLiteral("/Contents/MacOS/Gifiles"), marker);
#elif defined(Q_OS_WIN)
    const QString app = dir + QLatin1Char('/') + name;
    writeFile(app + QStringLiteral("/Gifiles.exe"), marker);
#else
    const QString app = dir + QLatin1Char('/') + name;
    writeFile(app, marker);
#endif
    return app;
}

QByteArray markerOf(const QString &app)
{
#if defined(Q_OS_MACOS)
    return readFile(app + QStringLiteral("/Contents/MacOS/Gifiles"));
#elif defined(Q_OS_WIN)
    return readFile(app + QStringLiteral("/Gifiles.exe"));
#else
    return readFile(app);
#endif
}

// A process id that no longer runs.
qint64 deadPid()
{
    QProcess p;
#ifdef Q_OS_WIN
    p.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("exit")});
#else
    p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 0")});
#endif
    p.waitForStarted();
    const qint64 pid = p.processId();
    p.waitForFinished();
    return pid;
}

} // namespace

class Unit : public QObject {
    Q_OBJECT

    // A published release in a folder: manifest/update.json(.sig) and assets/v<version>/<file>.
    struct Release {
        QString root;
        QByteArray manifest, asset;
        QString file;
    };

    // Builds the platform's package holding `marker` and its signed update.json.
    static Release publish(const QString &root, const QString &version, const QByteArray &marker)
    {
        Release r;
        r.root = root;
        const QString pkg = root + QStringLiteral("/pkg");
#if defined(Q_OS_MACOS)
        r.file = QStringLiteral("Gifiles-%1-macos.zip").arg(version);
        makeApp(pkg, QStringLiteral("Gifiles"), marker);
        const QString out = root + QStringLiteral("/assets/v%1/%2").arg(version, r.file);
        QDir().mkpath(QFileInfo(out).absolutePath());
        runTool(QStringLiteral("/usr/bin/ditto"), {QStringLiteral("-c"), QStringLiteral("-k"), QStringLiteral("--keepParent"),
                                                   pkg + QStringLiteral("/Gifiles.app"), out});
#elif defined(Q_OS_WIN)
        r.file = QStringLiteral("Gifiles-%1-windows-x64.zip").arg(version);
        makeApp(pkg, QStringLiteral("Gifiles"), marker);
        const QString out = root + QStringLiteral("/assets/v%1/%2").arg(version, r.file);
        QDir().mkpath(QFileInfo(out).absolutePath());
        runTool(QStringLiteral("tar.exe"), {QStringLiteral("-a"), QStringLiteral("-c"), QStringLiteral("-f"),
                                            QDir::toNativeSeparators(out), QStringLiteral("Gifiles")}, pkg);
#else
        r.file = QStringLiteral("Gifiles-%1-linux-x86_64.AppImage").arg(version);
        const QString out = root + QStringLiteral("/assets/v%1/%2").arg(version, r.file);
        writeFile(out, marker);
#endif
        r.asset = readFile(out);
        QJsonObject asset{{QStringLiteral("file"), r.file},
                          {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(r.asset, QCryptographicHash::Sha256).toHex())},
                          {QStringLiteral("size"), r.asset.size()}};
        r.manifest = QJsonDocument(QJsonObject{{QStringLiteral("version"), version},
                                               {QStringLiteral("assets"), QJsonObject{{platform(), asset}}}})
                         .toJson(QJsonDocument::Compact);
        writeFile(root + QStringLiteral("/manifest/update.json"), r.manifest);
        writeFile(root + QStringLiteral("/manifest/update.json.sig"), sign(r.manifest).toBase64());
        return r;
    }

    static Updater::Config config(const QString &root)
    {
        Updater::Config c;
        c.manifestBase = QUrl::fromLocalFile(root + QStringLiteral("/manifest/"));
        c.assetBase = QUrl::fromLocalFile(root + QStringLiteral("/assets/"));
        c.publicKey = kPublic;
        c.version = QStringLiteral("0.1.5");
        c.platform = platform();
        c.target = root + QStringLiteral("/installed");
        c.staging = root + QStringLiteral("/staging");
        return c;
    }

    // Runs one check to its end.
    static Updater::State checkOnce(Updater &u)
    {
        u.check();
        if (u.state() != Updater::State::Checking && u.state() != Updater::State::Downloading)
            return u.state();
        QSignalSpy spy(&u, &Updater::stateChanged);
        for (int i = 0; i < 300 && (u.state() == Updater::State::Checking || u.state() == Updater::State::Downloading); ++i)
            spy.wait(100);
        return u.state();
    }

private slots:
    void signatureFromOpensslVerifies()
    {
        // What the release job does: openssl pkeyutl -sign -rawin with the Ed25519 key.
        const QByteArray msg = R"({"version":"9.9.9"})";
        const QByteArray sig = QByteArray::fromBase64(
            "BGeadA6KK07MXLEnyJtXAHIRnuGAfqb2Bq+nfJG9faDDTaBgd9guddUSrLON25rPcHNgUonRBtXQT9VNQj02DQ==");
        QVERIFY(Updater::verifySignature(msg, sig, kPublic));
        QVERIFY(Updater::verifySignature(msg, sign(msg), kPublic));
        QByteArray changed = msg;
        changed[2] = 'V';
        QVERIFY(!Updater::verifySignature(changed, sig, kPublic));
        QByteArray badSig = sig;
        badSig[10] = char(badSig[10] ^ 1);
        QVERIFY(!Updater::verifySignature(msg, badSig, kPublic));
        QVERIFY(!Updater::verifySignature(msg, sig.left(63), kPublic));
        QVERIFY(!Updater::verifySignature(msg, sig, kPublic.left(31)));
        QByteArray otherKey = kPublic;
        otherKey[0] = char(otherKey[0] ^ 1);
        QVERIFY(!Updater::verifySignature(msg, sig, otherKey));
    }

    void versionsCompareNumerically()
    {
        QVERIFY(Updater::isNewer(QStringLiteral("0.1.10"), QStringLiteral("0.1.9")));
        QVERIFY(Updater::isNewer(QStringLiteral("0.2.0"), QStringLiteral("0.1.99")));
        QVERIFY(Updater::isNewer(QStringLiteral("1.0"), QStringLiteral("0.9.9")));
        QVERIFY(!Updater::isNewer(QStringLiteral("0.1.9"), QStringLiteral("0.1.9")));
        QVERIFY(!Updater::isNewer(QStringLiteral("0.1.8"), QStringLiteral("0.1.9")));
        QVERIFY(!Updater::isNewer(QString(), QStringLiteral("0.1.9")));
        QVERIFY(!Updater::isNewer(QStringLiteral("abc"), QStringLiteral("0.1.9")));
    }

    void manifestIsValidated()
    {
        const QString sha(64, QLatin1Char('a'));
        auto json = [&](const QString &version, const QString &file, const QString &hash, qint64 size) {
            return QJsonDocument(QJsonObject{{QStringLiteral("version"), version},
                                             {QStringLiteral("assets"),
                                              QJsonObject{{QStringLiteral("linux-x86_64"),
                                                           QJsonObject{{QStringLiteral("file"), file},
                                                                       {QStringLiteral("sha256"), hash},
                                                                       {QStringLiteral("size"), size}}}}}})
                .toJson();
        };
        Updater::Manifest m = Updater::parseManifest(json(QStringLiteral("0.1.42"), QStringLiteral("Gifiles-0.1.42.AppImage"), sha, 10),
                                                     QStringLiteral("linux-x86_64"));
        QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
        QVERIFY(m.hasAsset);
        QCOMPARE(m.version, QStringLiteral("0.1.42"));
        QCOMPARE(m.file, QStringLiteral("Gifiles-0.1.42.AppImage"));
        QCOMPARE(m.sha256.size(), 32);
        QCOMPARE(m.size, 10);

        // Another platform's release has no file for this one: not an error.
        m = Updater::parseManifest(json(QStringLiteral("0.1.42"), QStringLiteral("x"), sha, 10), QStringLiteral("macos"));
        QVERIFY(m.error.isEmpty());
        QVERIFY(!m.hasAsset);

        // A file name that could leave the release folder, a wrong hash, a size or a version is refused.
        for (const QString &bad : {QStringLiteral("../evil"), QStringLiteral("a/b"), QStringLiteral("..x"), QStringLiteral(".hidden"),
                                   QStringLiteral(""), QStringLiteral("a b")})
            QVERIFY2(!Updater::parseManifest(json(QStringLiteral("0.1.42"), bad, sha, 10), QStringLiteral("linux-x86_64")).error.isEmpty(),
                     qPrintable(bad));
        QVERIFY(!Updater::parseManifest(json(QStringLiteral("0.1.42"), QStringLiteral("f"), sha.left(62), 10), QStringLiteral("linux-x86_64")).error.isEmpty());
        QVERIFY(!Updater::parseManifest(json(QStringLiteral("0.1.42"), QStringLiteral("f"), sha, 0), QStringLiteral("linux-x86_64")).error.isEmpty());
        QVERIFY(!Updater::parseManifest(json(QStringLiteral("0.1.x"), QStringLiteral("f"), sha, 10), QStringLiteral("linux-x86_64")).error.isEmpty());
        QVERIFY(!Updater::parseManifest("not json", QStringLiteral("linux-x86_64")).error.isEmpty());
    }

    void newerReleaseIsDownloadedCheckedAndUnpacked()
    {
        QTemporaryDir tmp;
        publish(tmp.path(), QStringLiteral("0.2.0"), "new version");
        Updater u(config(tmp.path()));
        QCOMPARE(checkOnce(u), Updater::State::Ready);
        QCOMPARE(u.readyVersion(), QStringLiteral("0.2.0"));
        QCOMPARE(u.latestVersion(), QStringLiteral("0.2.0"));
        // What will replace the app is unpacked, and the download itself is gone.
        const QString unpacked = tmp.path() + QStringLiteral("/staging/0.2.0/unpacked");
#if defined(Q_OS_MACOS)
        QCOMPARE(markerOf(unpacked + QStringLiteral("/Gifiles.app")), QByteArray("new version"));
#elif defined(Q_OS_WIN)
        QCOMPARE(markerOf(unpacked + QStringLiteral("/Gifiles")), QByteArray("new version"));
#else
        QCOMPARE(markerOf(unpacked + QStringLiteral("/Gifiles.AppImage")), QByteArray("new version"));
        QVERIFY(QFileInfo(unpacked + QStringLiteral("/Gifiles.AppImage")).isExecutable());
#endif
        QCOMPARE(QDir(tmp.path() + QStringLiteral("/staging/0.2.0")).entryList(QDir::Files), QStringList());

        // Checking again with the same release keeps the download.
        QCOMPARE(checkOnce(u), Updater::State::Ready);
        QCOMPARE(u.readyVersion(), QStringLiteral("0.2.0"));
    }

    void sameOrOlderReleaseIsNotDownloaded()
    {
        for (const QString &v : {QStringLiteral("0.1.5"), QStringLiteral("0.1.4")}) {
            QTemporaryDir tmp;
            publish(tmp.path(), v, "same");
            Updater u(config(tmp.path()));
            QCOMPARE(checkOnce(u), Updater::State::Idle);
            QVERIFY(u.readyVersion().isEmpty());
            QVERIFY(!QFileInfo::exists(tmp.path() + QStringLiteral("/staging")));
        }
    }

    void releaseWithoutThisPlatformIsNotAnUpdate()
    {
        QTemporaryDir tmp;
        publish(tmp.path(), QStringLiteral("0.2.0"), "x");
        Updater::Config c = config(tmp.path());
        c.platform = QStringLiteral("some-other-platform");
        Updater u(c);
        QCOMPARE(checkOnce(u), Updater::State::Idle);
        QCOMPARE(u.latestVersion(), QStringLiteral("0.2.0"));
    }

    void wrongSignatureIsRefused()
    {
        QTemporaryDir tmp;
        const Release r = publish(tmp.path(), QStringLiteral("0.2.0"), "x");
        // A manifest changed after signing (say, pointing at another file) doesn't verify.
        QByteArray changed = r.manifest;
        changed.replace("0.2.0", "0.3.0");
        writeFile(tmp.path() + QStringLiteral("/manifest/update.json"), changed);
        Updater u(config(tmp.path()));
        QCOMPARE(checkOnce(u), Updater::State::Failed);
        QVERIFY(u.readyVersion().isEmpty());
        QVERIFY(!QFileInfo::exists(tmp.path() + QStringLiteral("/staging")));

        // Signed with another key: refused too.
        writeFile(tmp.path() + QStringLiteral("/manifest/update.json"), r.manifest);
        Updater::Config c = config(tmp.path());
        c.publicKey[5] = char(c.publicKey[5] ^ 1);
        Updater other(c);
        QCOMPARE(checkOnce(other), Updater::State::Failed);
    }

    void changedDownloadIsRefused()
    {
        QTemporaryDir tmp;
        const Release r = publish(tmp.path(), QStringLiteral("0.2.0"), "original");
        const QString asset = tmp.path() + QStringLiteral("/assets/v0.2.0/") + r.file;
        // Same size, other bytes: the SHA-256 doesn't match.
        QByteArray changed = r.asset;
        changed[changed.size() / 2] = char(changed[changed.size() / 2] ^ 0x55);
        writeFile(asset, changed);
        Updater u(config(tmp.path()));
        QCOMPARE(checkOnce(u), Updater::State::Failed);
        QVERIFY(u.readyVersion().isEmpty());
        // Longer than the manifest says: stopped.
        writeFile(asset, r.asset + "more");
        QCOMPARE(checkOnce(u), Updater::State::Failed);
        QVERIFY(u.readyVersion().isEmpty());
        // Missing: reported, not ready.
        QFile::remove(asset);
        QCOMPARE(checkOnce(u), Updater::State::Failed);
    }

    void unreachableReleaseIsReported()
    {
        QTemporaryDir tmp;
        Updater u(config(tmp.path())); // nothing published
        QCOMPARE(checkOnce(u), Updater::State::Failed);
        QVERIFY(!u.error().isEmpty());
    }

    void offBuildNeverChecks()
    {
        QTemporaryDir tmp;
        publish(tmp.path(), QStringLiteral("0.2.0"), "x");
        Updater::Config c = config(tmp.path());
        c.offReason = QStringLiteral("off");
        Updater u(c);
        QCOMPARE(u.state(), Updater::State::Off);
        u.check();
        QCOMPARE(u.state(), Updater::State::Off);
        QVERIFY(!u.apply(false));
        QVERIFY(!QFileInfo::exists(tmp.path() + QStringLiteral("/staging")));
    }

    void macAppMustBeSignedByTheTeam()
    {
#ifndef Q_OS_MACOS
        QSKIP("macOS only");
#else
        QTemporaryDir tmp;
        publish(tmp.path(), QStringLiteral("0.2.0"), "unsigned");
        Updater::Config c = config(tmp.path());
        c.teamId = QStringLiteral("AF68GKBM82");
        Updater u(c);
        QCOMPARE(checkOnce(u), Updater::State::Failed);
        QVERIFY(u.readyVersion().isEmpty());
#endif
    }

    void helperSwapsTheNewVersionIn()
    {
        QTemporaryDir tmp;
        const QString target = makeApp(tmp.path(), QStringLiteral("Installed App"), "v1");
        const QString staged = makeApp(tmp.path() + QStringLiteral("/staging dir"), QStringLiteral("Gifiles"), "v2");
        const QString script = Updater::writeHelper(tmp.path() + QStringLiteral("/staging dir"));
        QVERIFY(!script.isEmpty());
        const auto [program, args] = Updater::helperCommand(script, deadPid(), staged, target, false);
        QVERIFY2(runTool(program, args, tmp.path()), qPrintable(args.join(QLatin1Char(' '))));
        QCOMPARE(markerOf(target), QByteArray("v2"));
        QVERIFY(!QFileInfo::exists(target + QStringLiteral(".old")));
        QVERIFY(!QFileInfo::exists(target + QStringLiteral(".new")));
        QVERIFY(!QFileInfo::exists(staged));
#ifdef Q_OS_LINUX
        QVERIFY(QFileInfo(target).isExecutable());
#endif
    }

    void helperKeepsTheOldVersionWhenThereIsNothingToInstall()
    {
        QTemporaryDir tmp;
        const QString target = makeApp(tmp.path(), QStringLiteral("Installed"), "v1");
        const QString script = Updater::writeHelper(tmp.path() + QStringLiteral("/staging"));
        const auto [program, args] =
            Updater::helperCommand(script, deadPid(), tmp.path() + QStringLiteral("/staging/missing"), target, false);
        QVERIFY(!runTool(program, args, tmp.path()));
        QCOMPARE(markerOf(target), QByteArray("v1"));
        QVERIFY(!QFileInfo::exists(target + QStringLiteral(".old")));
        QVERIFY(!QFileInfo::exists(target + QStringLiteral(".new")));
    }

    void helperWaitsForTheAppToExit()
    {
        QTemporaryDir tmp;
        const QString target = makeApp(tmp.path(), QStringLiteral("Installed"), "v1");
        const QString staged = makeApp(tmp.path() + QStringLiteral("/staging"), QStringLiteral("Gifiles"), "v2");
        const QString script = Updater::writeHelper(tmp.path() + QStringLiteral("/staging"));
        // A stand-in for the running app: the helper must not touch the target while it runs.
        QProcess app;
#ifdef Q_OS_WIN
        app.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Start-Sleep -Seconds 30")});
#else
        app.start(QStringLiteral("/bin/sleep"), {QStringLiteral("30")});
#endif
        QVERIFY(app.waitForStarted());
        const auto [program, args] = Updater::helperCommand(script, app.processId(), staged, target, false);
        QProcess helper;
        helper.start(program, args);
        QVERIFY(helper.waitForStarted());
        QVERIFY(!helper.waitForFinished(1500)); // still waiting
        QCOMPARE(markerOf(target), QByteArray("v1"));
        app.kill();
        app.waitForFinished();
        QVERIFY(helper.waitForFinished(30000));
        QCOMPARE(helper.exitCode(), 0);
        QCOMPARE(markerOf(target), QByteArray("v2"));
    }

    void applyNeedsAReadyUpdate()
    {
        QTemporaryDir tmp;
        Updater u(config(tmp.path()));
        QVERIFY(!u.apply(false)); // nothing downloaded
    }
};

QTEST_GUILESS_MAIN(Unit)
#include "unit_update.moc"
