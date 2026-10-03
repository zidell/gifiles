#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

// A child process (the user's shell) attached to a pseudo terminal.
// Unix: forkpty. Windows: ConPTY.
class Pty : public QObject {
    Q_OBJECT
public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override;

    // program/args: e.g. "/bin/zsh" with argv0 "-zsh" for a login shell.
    bool start(const QString &program, const QStringList &args, const QString &argv0, const QString &cwd,
               int cols, int rows, const QStringList &extraEnv);
    bool isRunning() const;
    void write(const QByteArray &data);
    void resize(int cols, int rows);

    // True when the shell itself is in the foreground (sitting at its prompt, no command running).
    bool isShellIdle() const;
    // The shell's working directory, or empty if the platform can't tell (Windows uses OSC 7 instead).
    QString shellCwd() const;

signals:
    void dataReceived(const QByteArray &data);
    void finished();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
