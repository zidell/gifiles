#pragma once

#include <QElapsedTimer>
#include <QFont>
#include <QPoint>
#include <QTimer>
#include <QVector>
#include <QWidget>

extern "C" {
#include <vterm.h>
}

class Pty;

// An embedded terminal (libvterm + the user's shell) wired to the file browser:
//  - followFolder(): cd's along with the browser, but only when the shell is idle and the
//    command line is empty (otherwise it waits);
//  - items dragged from the list are typed in as quoted paths — "rm -rf " + drag files + Enter;
//  - runCommand(): runs a command line (e.g. the AI request) as soon as the shell is free;
//  - cwdChanged(): the shell's own cd's are reported so the browser can follow back.
class TerminalWidget : public QWidget {
    Q_OBJECT
public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    void start(const QString &cwd);
    bool isRunning() const;
    bool isReady() const; // the shell has reached its first prompt
    void followFolder(const QString &path);
    // Runs a command once the shell is idle and the command line is empty (never clobbering
    // what the user is typing), after any pending cd.
    void runCommand(const QString &command);
    // Quotes free text (may span lines) as one shell word.
    QString quoteText(const QString &text) const { return quoteWord(text); }
    // One shell word for any text — spaces, quotes, $, `, Korean/NFD names, newlines and other
    // control characters — kept on one line: $'...' (zsh, bash) or "..." with backticks (PowerShell).
    static QString quoteWord(const QString &text);
    // A user command from "선택한 항목들로…" with its placeholders filled in, each item quoted:
    // {files} absolute paths, {names} paths relative to {dir}, {dir} the first item's folder,
    // (`folder` when nothing is selected), {prompt} the one-line request. Several items are
    // separated by spaces (sh) or commas (PowerShell, an array).
    static QString expandCommand(const QString &tmpl, const QStringList &paths, const QString &prompt,
                                 const QString &folder = {});
    QString shellCwd() const { return m_cwd; }
    QString screenText() const; // visible screen as plain text (tests, accessibility)
    QString title() const;
    QString tabTitle() const; // short: the program's title if it set one, else the folder name
    // A program runs in the foreground (not just a half-typed line). A shell still starting up
    // isn't busy: a cd waits for its first prompt.
    bool isBusy() const;
    bool isAt(const QString &path) const;

signals:
    void cwdChanged(const QString &path);
    void titleChanged();
    // Configurable terminal tab keys (Cmd on macOS, Ctrl on Windows/Linux).
    void newTabRequested();
    void closeRequested();

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    friend struct TerminalCallbacks;
    using Line = QVector<VTermScreenCell>;

    void applyTheme();
    void updateFont();
    void recomputeSize();
    void sendBytes(const QByteArray &bytes, bool userTyped);
    void sendText(const QString &text, bool userTyped);
    void sendKey(VTermKey key, VTermModifier mod);
    void paste(const QString &text);
    void onUserInput();
    void lineSubmitted();
    bool lineInUse() const; // something may be on the command line: never type over it
    void tryPendingCd();
    void pollCwd();
    // The shell reports a new directory (polled, or OSC 7): moves the browser unless it is the
    // echo of our own cd (the browser is there already, or has moved on since).
    void shellMovedTo(const QString &cwd);
    bool handleShortcut(QKeyEvent *e, bool dryRun);
    QString quote(const QString &path) const;
    void flushOutput();

    // Cells by absolute line: 0..scrollback-1 are history, then the live screen.
    int totalLines() const { return int(m_scrollback.size()) + m_rows; }
    bool cellAt(int line, int col, VTermScreenCell *cell) const;
    QPoint cellFromPos(const QPoint &pos) const; // (col, absolute line)
    QString textBetween(QPoint a, QPoint b) const;
    QString selectedText() const;
    bool isSelected(int line, int col) const;

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    Pty *m_pty = nullptr;
    QString m_startCwd;
    int m_rows = 24, m_cols = 80;
    QFont m_font, m_bold;
    int m_cellW = 8, m_cellH = 16, m_ascent = 12;
    QVector<Line> m_scrollback;
    int m_scrollOffset = 0; // lines scrolled back into history
    qreal m_wheelAccum = 0;
    VTermPos m_cursor{0, 0};
    bool m_cursorVisible = true;
    int m_cursorShape = VTERM_PROP_CURSORSHAPE_BLOCK;
    bool m_altScreen = false;
    int m_mouseMode = VTERM_PROP_MOUSE_NONE;
    QString m_title;
    QString m_preedit;
    QColor m_fg, m_bg;

    // Selection (col, absolute line)
    QPoint m_selStart{-1, -1}, m_selEnd{-1, -1};
    bool m_selecting = false;

    // Command-line tracking for the browser integration.
    QString m_typed;      // what the user typed since the last Enter (best effort)
    bool m_lineRecalled = false; // ↑/↓, Tab, ⌃R… may have put text on the line that m_typed doesn't know
    QString m_pendingCd;  // folder to cd to once the shell is idle and the line is empty
    QString m_pendingCommand;
    void tryPendingCommand();
    QString m_cdTarget;   // the cd we sent and are waiting to see arrive
    QElapsedTimer m_cdSentAt;
    QString m_cwd;
    bool m_promptSeen = false; // the shell has been idle at least once since start
    QTimer m_poll;
    QByteArray m_outBuf;  // bytes produced by libvterm for the pty
};
