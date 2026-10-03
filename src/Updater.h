#pragma once

#include <QByteArray>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;
class QTimer;

// Automatic updates of the published builds (GitHub releases of zidell/gifiles): a signed
// update.json names the newest version and each platform's file with its SHA-256. The new version
// is downloaded and checked in the background, then a small helper swaps it in once the app has
// quit (on quit, or "restart to update"). Builds made by hand (no GIFILES_UPDATES) never update.
class Updater : public QObject {
    Q_OBJECT
public:
    struct Config {
        QUrl manifestBase;    // folder with update.json and update.json.sig
        QUrl assetBase;       // a release's files: <assetBase>v<version>/<file>
        QByteArray publicKey; // Ed25519, 32 bytes: update.json.sig must verify against it
        QString version;      // the running version
        QString platform;     // the asset key: "macos", "windows-x64", "linux-x86_64"
        QString target;       // what is replaced: Gifiles.app, the folder with Gifiles.exe, the AppImage
        QString staging;      // download and unpack folder (owned by the updater)
        QString teamId;       // macOS: the new app must be signed by this team ("" = not checked)
        QString offReason;    // non-empty: updates are off, and why (shown to the user)
    };

    struct Manifest {
        QString version, file, error; // error: why the manifest can't be used ("" = fine)
        QByteArray sha256;            // raw 32 bytes
        qint64 size = 0;
        bool hasAsset = false;        // false: no file for this platform in that release
    };

    enum class State { Off, Idle, Checking, Downloading, Ready, Failed };

    static Updater *instance(); // configured for this build, the platform and config.toml
    // Called by main() in the published packages (GIFILES_UPDATES) before instance(); without it,
    // as in tests and builds made by hand, updates are off.
    static void allowUpdates();
    explicit Updater(const Config &config, QObject *parent = nullptr);

    State state() const { return m_state; }
    QString readyVersion() const { return m_readyVersion; }
    QString latestVersion() const { return m_latest; } // from the last successful check
    QString error() const { return m_error; }
    QString offReason() const { return m_config.offReason; }
    QString version() const { return m_config.version; }

    void check(); // fetch, verify, download, unpack; ends in Idle (up to date), Ready or Failed
    // Starts the helper that waits for this process to exit and puts the staged version in place.
    // Only from Ready; once per process.
    bool apply(bool relaunch);
    void setAutomatic(bool on); // periodic checks (every 6 hours, the first a minute after start)

    static bool verifySignature(const QByteArray &message, const QByteArray &signature, const QByteArray &publicKey);
    static Manifest parseManifest(const QByteArray &json, const QString &platform);
    static bool isNewer(const QString &candidate, const QString &current);
    // The helper: its script (written to `dir`) and the command that runs it.
    static QString writeHelper(const QString &dir);
    static QPair<QString, QStringList> helperCommand(const QString &script, qint64 pid, const QString &staged,
                                                     const QString &target, bool relaunch);

signals:
    void stateChanged();

private:
    void setState(State s, const QString &error = {});
    void fetched(const QByteArray &manifest, const QByteArray &signature);
    void download(const Manifest &m);
    void unpack(const Manifest &m, const QString &file);

    Config m_config;
    State m_state = State::Idle;
    QString m_error, m_readyVersion, m_latest, m_staged;
    bool m_applied = false;
    QNetworkAccessManager *m_net = nullptr;
    QTimer *m_timer = nullptr;
};
