#include "TerminalWidget.h"
#include "Util.h"
#include "Log.h"
#include "Shortcuts.h"
#include "Pty.h"
#include "Settings.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QUrl>
#include <QtMath>

#ifndef Q_OS_WIN
#include <pwd.h>
#include <unistd.h>
#endif

namespace {
constexpr int kPad = 6;
constexpr int kMaxScrollback = 5000;

#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier kCtrl = Qt::MetaModifier; // the physical Control key
constexpr Qt::KeyboardModifier kCmd = Qt::ControlModifier; // ⌘
#else
constexpr Qt::KeyboardModifier kCtrl = Qt::ControlModifier;
constexpr Qt::KeyboardModifier kCmd = Qt::NoModifier;
#endif

QString defaultShell()
{
    const QString configured = Settings::instance()->value(Settings::TermShell).toString().trimmed();
    if (!configured.isEmpty())
        return configured;
#ifdef Q_OS_WIN
    return QStringLiteral("powershell.exe");
#else
    QString sh = qEnvironmentVariable("SHELL");
    if (sh.isEmpty())
        if (const passwd *pw = getpwuid(getuid()); pw && pw->pw_shell)
            sh = QString::fromLocal8Bit(pw->pw_shell);
    if (sh.isEmpty())
        sh = QStringLiteral("/bin/zsh");
    return sh;
#endif
}

// libvterm marks the right half of a double-width character (Hangul, CJK, emoji) with this.
constexpr uint32_t kWideContinuation = 0xFFFFFFFFu;

bool isContinuation(const VTermScreenCell &cell)
{
    return cell.width == 0 || cell.chars[0] == kWideContinuation;
}

// The cell's characters as display text. macOS file names hold Hangul decomposed into jamo
// (NFD); composing them (NFC) gives proper syllables.
QString cellText(const VTermScreenCell &cell)
{
    QString s;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] && cell.chars[i] != kWideContinuation; ++i)
        s += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[i]), 1);
    return s.normalized(QString::NormalizationForm_C);
}

QString canonical(const QString &p)
{
    const QString c = QFileInfo(p).canonicalFilePath();
    return c.isEmpty() ? QDir::cleanPath(p) : c;
}
} // namespace

// libvterm calls back into the widget through these.
struct TerminalCallbacks {
    static TerminalWidget *w(void *u) { return static_cast<TerminalWidget *>(u); }

    static int damage(VTermRect, void *u)
    {
        w(u)->update();
        return 1;
    }
    static int moverect(VTermRect, VTermRect, void *u)
    {
        w(u)->update();
        return 1;
    }
    static int movecursor(VTermPos pos, VTermPos, int visible, void *u)
    {
        w(u)->m_cursor = pos;
        w(u)->m_cursorVisible = visible;
        w(u)->update();
        return 1;
    }
    static int settermprop(VTermProp prop, VTermValue *val, void *u)
    {
        TerminalWidget *t = w(u);
        switch (prop) {
        case VTERM_PROP_CURSORVISIBLE: t->m_cursorVisible = val->boolean; break;
        case VTERM_PROP_CURSORSHAPE: t->m_cursorShape = val->number; break;
        case VTERM_PROP_ALTSCREEN: t->m_altScreen = val->boolean; t->m_scrollOffset = 0; break;
        case VTERM_PROP_MOUSE: t->m_mouseMode = val->number; break;
        case VTERM_PROP_TITLE: {
            static QByteArray buf;
            if (val->string.initial)
                buf.clear();
            buf.append(val->string.str, int(val->string.len));
            if (val->string.final) {
                t->m_title = QString::fromUtf8(buf);
                emit t->titleChanged();
            }
            break;
        }
        default: break;
        }
        t->update();
        return 1;
    }
    static int bell(void *)
    {
        // Consume BEL silently: shells can ring it while our automatic cd clears the prompt.
        return 1;
    }
    static int pushline(int cols, const VTermScreenCell *cells, void *u)
    {
        TerminalWidget *t = w(u);
        t->m_scrollback.append(TerminalWidget::Line(cells, cells + cols));
        if (t->m_scrollback.size() > kMaxScrollback)
            t->m_scrollback.removeFirst();
        else if (t->m_scrollOffset > 0)
            ++t->m_scrollOffset; // keep the history view still while output arrives
        return 1;
    }
    static int popline(int cols, VTermScreenCell *cells, void *u)
    {
        TerminalWidget *t = w(u);
        if (t->m_scrollback.isEmpty())
            return 0;
        const TerminalWidget::Line line = t->m_scrollback.takeLast();
        VTermScreenCell blank = {};
        blank.width = 1;
        blank.fg.type = VTERM_COLOR_INDEXED | VTERM_COLOR_DEFAULT_FG;
        blank.bg.type = VTERM_COLOR_INDEXED | VTERM_COLOR_DEFAULT_BG;
        for (int i = 0; i < cols; ++i)
            cells[i] = i < line.size() ? line[i] : blank;
        t->m_scrollOffset = qMin(t->m_scrollOffset, int(t->m_scrollback.size()));
        return 1;
    }
    static int sbclear(void *u)
    {
        w(u)->m_scrollback.clear();
        w(u)->m_scrollOffset = 0;
        return 1;
    }
    // OSC 7 "file://host/path": shells (and our PowerShell prompt) announce their directory.
    static int osc(int command, VTermStringFragment frag, void *u)
    {
        static QByteArray buf;
        if (command != 7)
            return 0;
        if (frag.initial)
            buf.clear();
        buf.append(frag.str, int(frag.len));
        if (frag.final) {
            const QString path = QUrl(QString::fromUtf8(buf)).toLocalFile();
            if (!path.isEmpty())
                w(u)->m_promptSeen = true;
            w(u)->shellMovedTo(path);
        }
        return 1;
    }
    static void output(const char *s, size_t len, void *u) { w(u)->m_outBuf.append(s, qsizetype(len)); }
};

TerminalWidget::TerminalWidget(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAcceptDrops(true);
    setCursor(Qt::IBeamCursor);
    setMinimumHeight(60);

    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, &TerminalCallbacks::output, this);
    m_screen = vterm_obtain_screen(m_vt);
    static const VTermScreenCallbacks cbs = {
        &TerminalCallbacks::damage, &TerminalCallbacks::moverect, &TerminalCallbacks::movecursor,
        &TerminalCallbacks::settermprop, &TerminalCallbacks::bell, nullptr,
        &TerminalCallbacks::pushline, &TerminalCallbacks::popline, &TerminalCallbacks::sbclear,
    };
    vterm_screen_set_callbacks(m_screen, &cbs, this);
    static const VTermStateFallbacks fallbacks = {nullptr, nullptr, &TerminalCallbacks::osc, nullptr, nullptr, nullptr, nullptr};
    vterm_screen_set_unrecognised_fallbacks(m_screen, &fallbacks, this);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_enable_reflow(m_screen, true);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_reset(m_screen, 1);

    updateFont();
    applyTheme();
    connect(Theme::instance(), &Theme::changed, this, &TerminalWidget::applyTheme);
    connect(Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String(Settings::TermFontSize))
            updateFont();
    });
    m_poll.setInterval(400);
    connect(&m_poll, &QTimer::timeout, this, [this] {
#ifndef Q_OS_WIN
        // Unix can inspect the foreground process group. On Windows only the
        // prompt's OSC 7 establishes readiness; no children also describes startup.
        if (!m_promptSeen && m_pty && m_pty->isShellIdle())
            m_promptSeen = true;
#endif
        pollCwd();
        tryPendingCd();
        tryPendingCommand();
    });
}

TerminalWidget::~TerminalWidget()
{
    delete m_pty; // ends the shell before libvterm goes away
    m_pty = nullptr;
    vterm_free(m_vt);
}

void TerminalWidget::applyTheme()
{
    const Theme::Colors &c = Theme::colors();
    m_fg = c.text;
    m_bg = c.dark ? QColor(0x17, 0x17, 0x19) : QColor(0xFB, 0xFB, 0xFC);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, m_fg.red(), m_fg.green(), m_fg.blue());
    vterm_color_rgb(&bg, m_bg.red(), m_bg.green(), m_bg.blue());
    vterm_screen_set_default_colors(m_screen, &fg, &bg);
    static const char *dark[16] = {"#4c4c50", "#ff6b6b", "#63d471", "#f2c94c", "#5aa9ff", "#c678dd", "#56c8d8", "#d4d4d8",
                                   "#6e6e73", "#ff8787", "#86e597", "#f7d774", "#82bfff", "#d79ef0", "#7fdbe8", "#ffffff"};
    static const char *light[16] = {"#1d1d1f", "#c62828", "#2e7d32", "#9a6700", "#1f5fcf", "#8e24aa", "#00838f", "#8e8e93",
                                    "#48484a", "#e53935", "#43a047", "#b38600", "#2f6fde", "#ab47bc", "#0097a7", "#3a3a3c"};
    VTermState *state = vterm_obtain_state(m_vt);
    for (int i = 0; i < 16; ++i) {
        const QColor q(QString::fromLatin1(c.dark ? dark[i] : light[i]));
        VTermColor col;
        vterm_color_rgb(&col, q.red(), q.green(), q.blue());
        vterm_state_set_palette_color(state, i, &col);
    }
    update();
}

void TerminalWidget::updateFont()
{
    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setPointSize(Settings::instance()->value(Settings::TermFontSize).toInt());
    m_font.setStyleHint(QFont::Monospace);
    m_bold = m_font;
    m_bold.setBold(true);
    const QFontMetricsF fm(m_font);
    m_cellW = qCeil(fm.horizontalAdvance(QLatin1Char('M')));
    m_cellH = qCeil(fm.height()) + 2;
    m_ascent = qCeil(fm.ascent()) + 1;
    recomputeSize();
    update();
}

void TerminalWidget::recomputeSize()
{
    const int cols = qMax(10, (width() - 2 * kPad) / m_cellW);
    const int rows = qMax(2, (height() - 2 * kPad) / m_cellH);
    if (cols == m_cols && rows == m_rows)
        return;
    m_cols = cols;
    m_rows = rows;
    vterm_set_size(m_vt, rows, cols);
    vterm_screen_flush_damage(m_screen);
    if (m_pty)
        m_pty->resize(cols, rows);
    m_scrollOffset = qMin(m_scrollOffset, int(m_scrollback.size()));
}

void TerminalWidget::resizeEvent(QResizeEvent *)
{
    recomputeSize();
}

// ---------------------------------------------------------------------------
// Shell process

void TerminalWidget::start(const QString &cwd)
{
    delete m_pty;
    m_startCwd = cwd;
    m_cwd = canonical(cwd);
    m_typed.clear();
    m_lineRecalled = false;
    m_promptSeen = false;
    m_pty = new Pty(this);
    const QString shell = defaultShell();
    QStringList args;
    QString argv0;
#ifdef Q_OS_WIN
    // Report the location with OSC 7 at every prompt so the browser can follow `cd`.
    args << QStringLiteral("-NoLogo") << QStringLiteral("-NoExit") << QStringLiteral("-Command")
         << QStringLiteral("function global:prompt { $p = $executionContext.SessionState.Path.CurrentLocation.ProviderPath; "
                           "[char]27 + ']7;file:///' + ($p -replace '\\\\','/') + [char]7 + 'PS ' + $p + '> ' }");
#else
    argv0 = QLatin1Char('-') + QFileInfo(shell).fileName(); // login shell, like Terminal.app
#endif
    QStringList env{QStringLiteral("TERM=xterm-256color"), QStringLiteral("COLORTERM=truecolor"),
                    QStringLiteral("TERM_PROGRAM=Gifiles")};
    // Apps started from Finder/Dock get no locale; without it shells mangle Korean file names.
    if (qEnvironmentVariableIsEmpty("LANG") && qEnvironmentVariableIsEmpty("LC_ALL")) {
        QString lang = QLocale::system().name() + QStringLiteral(".UTF-8");
#ifndef Q_OS_WIN
        if (!QFileInfo::exists(QStringLiteral("/usr/share/locale/") + lang) &&
            !QFileInfo::exists(QStringLiteral("/usr/lib/locale/") + lang))
            lang = QStringLiteral("en_US.UTF-8"); // any UTF-8 locale keeps non-ASCII names intact
#endif
        env << QStringLiteral("LANG=") + lang;
    }
    connect(m_pty, &Pty::dataReceived, this, [this](const QByteArray &data) {
        vterm_input_write(m_vt, data.constData(), size_t(data.size()));
        vterm_screen_flush_damage(m_screen);
        flushOutput(); // terminal replies (device attributes, cursor reports)
    });
    connect(m_pty, &Pty::finished, this, [this] {
        const QByteArray msg = ("\r\n\x1b[2m[" + Gifiles::tr("프로세스가 종료됨 — Enter를 누르면 다시 시작") + "]\x1b[0m\r\n").toUtf8();
        vterm_input_write(m_vt, msg.constData(), size_t(msg.size()));
        vterm_screen_flush_damage(m_screen);
        m_poll.stop();
        update();
    });
    if (!m_pty->start(shell, args, argv0, cwd, m_cols, m_rows, env)) {
        const QByteArray msg = (Gifiles::tr("셸을 시작하지 못했습니다: %1").arg(shell) + QStringLiteral("\r\n")).toUtf8();
        vterm_input_write(m_vt, msg.constData(), size_t(msg.size()));
        vterm_screen_flush_damage(m_screen);
        return;
    }
    m_poll.start();
}

bool TerminalWidget::isRunning() const
{
    return m_pty && m_pty->isRunning();
}

bool TerminalWidget::isReady() const
{
    return isRunning() && m_promptSeen;
}

QString TerminalWidget::title() const
{
    QString dir = QDir::toNativeSeparators(m_cwd);
    const QString home = QDir::homePath();
    if (m_cwd.startsWith(home))
        dir = QStringLiteral("~") + QDir::toNativeSeparators(m_cwd.mid(home.size()));
    return QFileInfo(defaultShell()).completeBaseName() + QStringLiteral(" — ") + dir;
}

QString TerminalWidget::tabTitle() const
{
    if (!m_title.trimmed().isEmpty())
        return m_title.trimmed();
    if (m_cwd == QDir::homePath())
        return QStringLiteral("~");
    const QString name = QFileInfo(m_cwd).fileName();
    return name.isEmpty() ? QDir::toNativeSeparators(m_cwd) : name;
}

bool TerminalWidget::isBusy() const
{
    return isRunning() && m_promptSeen && !m_pty->isShellIdle();
}

bool TerminalWidget::isAt(const QString &path) const
{
    return canonical(path) == canonical(m_cwd);
}

void TerminalWidget::flushOutput()
{
    if (m_outBuf.isEmpty() || !m_pty)
        return;
    m_pty->write(m_outBuf);
    m_outBuf.clear();
}

void TerminalWidget::pollCwd()
{
    if (!m_pty)
        return;
    shellMovedTo(m_pty->shellCwd());
}

void TerminalWidget::shellMovedTo(const QString &cwd)
{
    if (cwd.isEmpty() || cwd == m_cwd)
        return;
    m_cwd = cwd;
    emit titleChanged();
    // Directories seen while our own cd is on its way, and its arrival, aren't the user's doing.
    if (m_cdSentAt.isValid() && m_cdSentAt.elapsed() < 3000) {
        if (canonical(cwd) == canonical(m_cdTarget))
            m_cdSentAt.invalidate();
        return;
    }
    emit cwdChanged(cwd);
}

// ---------------------------------------------------------------------------
// Browser integration

QString TerminalWidget::quote(const QString &path) const
{
    QString p = QDir::toNativeSeparators(path);
#ifdef Q_OS_WIN
    return QLatin1Char('\'') + p.replace(QLatin1String("'"), QLatin1String("''")) + QLatin1Char('\'');
#else
    return QLatin1Char('\'') + p.replace(QLatin1String("'"), QLatin1String("'\\''")) + QLatin1Char('\'');
#endif
}

QString TerminalWidget::quoteWord(const QString &text)
{
    QString o;
#ifdef Q_OS_WIN
    o += QLatin1Char('"');
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '`': o += QStringLiteral("``"); break;
        case '"': o += QStringLiteral("`\""); break;
        case '$': o += QStringLiteral("`$"); break;
        case '\n': o += QStringLiteral("`n"); break;
        case '\r': o += QStringLiteral("`r"); break;
        case '\t': o += QStringLiteral("`t"); break;
        case 0x201C: case 0x201D: case 0x201E: // PowerShell treats typographic quotes as quotes
            o += QLatin1Char('`');
            o += c;
            break;
        default:
            if (c.unicode() < 0x20 || c.unicode() == 0x7f)
                o += QStringLiteral("$([char]0x%1)").arg(uint(c.unicode()), 2, 16, QLatin1Char('0'));
            else
                o += c;
        }
    }
    return o + QLatin1Char('"');
#else
    // $'...' keeps the command on one line; a raw control character typed into the pty would act
    // (a CR runs the line), so they go in as escapes.
    o += QStringLiteral("$'");
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '\\': o += QStringLiteral("\\\\"); break;
        case '\'': o += QStringLiteral("\\'"); break;
        case '\n': o += QStringLiteral("\\n"); break;
        case '\r': o += QStringLiteral("\\r"); break;
        case '\t': o += QStringLiteral("\\t"); break;
        default:
            if (c.unicode() < 0x20 || c.unicode() == 0x7f)
                o += QStringLiteral("\\x%1").arg(uint(c.unicode()), 2, 16, QLatin1Char('0'));
            else
                o += c;
        }
    }
    return o + QLatin1Char('\'');
#endif
}

QString TerminalWidget::expandCommand(const QString &tmpl, const QStringList &paths, const QString &prompt, const QString &folder)
{
#ifdef Q_OS_WIN
    const QString sep = QStringLiteral(", ");
#else
    const QString sep = QStringLiteral(" ");
#endif
    const QString dir = paths.isEmpty() ? folder : QFileInfo(paths.first()).absolutePath();
    QStringList files, names;
    for (const QString &p : paths) {
        files << quoteWord(QDir::toNativeSeparators(p));
        // A name starting with '-' would read as an option ("-m" makes zip delete the originals).
        QString name = QDir(dir).relativeFilePath(p);
        if (name.startsWith(QLatin1Char('-')))
            name.prepend(QStringLiteral("./"));
        names << quoteWord(QDir::toNativeSeparators(name));
    }
    // One pass, so a value that happens to contain "{names}" isn't replaced again.
    static const QRegularExpression re(QStringLiteral("\\{(files|names|dir|prompt)\\}"));
    QString out;
    qsizetype last = 0;
    for (auto it = re.globalMatch(tmpl); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += tmpl.mid(last, m.capturedStart() - last);
        const QString what = m.captured(1);
        if (what == QLatin1String("files"))
            out += files.join(sep);
        else if (what == QLatin1String("names"))
            out += names.join(sep);
        else if (what == QLatin1String("dir"))
            out += quoteWord(QDir::toNativeSeparators(dir));
        else
            out += quoteWord(prompt);
        last = m.capturedEnd();
    }
    return out + tmpl.mid(last);
}

void TerminalWidget::runCommand(const QString &command)
{
    m_pendingCommand = command;
    tryPendingCommand();
}

void TerminalWidget::tryPendingCommand()
{
    if (m_pendingCommand.isEmpty() || !isReady() || !m_pendingCd.isEmpty())
        return;
    if (lineInUse() || !m_pty->isShellIdle())
        return; // retried by the poll timer
    Log::write("terminal", QStringLiteral("run queued command (%1 chars)").arg(m_pendingCommand.size()));
    sendBytes(m_pendingCommand.toUtf8() + "\r", false);
    m_scrollOffset = 0;
    m_pendingCommand.clear();
}

void TerminalWidget::followFolder(const QString &path)
{
    if (!Settings::instance()->flag(Settings::TermFollowFolder))
        return;
    m_pendingCd = path;
    tryPendingCd();
}

void TerminalWidget::tryPendingCd()
{
    if (m_pendingCd.isEmpty() || !isReady())
        return;
    if (canonical(m_pendingCd) == canonical(m_cwd)) {
        m_pendingCd.clear();
        return;
    }
    // Never disturb a running command or a half-typed command line; retried by the poll timer.
    if (lineInUse() || !m_pty->isShellIdle())
        return;
#ifdef Q_OS_WIN
    const QByteArray cmd = "\x1b Set-Location -LiteralPath " + quote(m_pendingCd).toUtf8() + "\r";
#else
    const QByteArray cmd = "\x15 cd -- " + quote(m_pendingCd).toUtf8() + "\r";
#endif
    sendBytes(cmd, false);
    m_scrollOffset = 0;
    m_cdTarget = m_pendingCd;
    m_cdSentAt.start();
    m_pendingCd.clear();
    tryPendingCommand();
}

void TerminalWidget::onUserInput()
{
    m_scrollOffset = 0;
    m_selStart = m_selEnd = QPoint(-1, -1);
}

bool TerminalWidget::lineInUse() const
{
    return !m_typed.isEmpty() || m_lineRecalled;
}

void TerminalWidget::lineSubmitted()
{
    m_typed.clear();
    m_lineRecalled = false;
    QTimer::singleShot(150, this, [this] {
        tryPendingCd();
        tryPendingCommand();
    });
}

void TerminalWidget::sendBytes(const QByteArray &bytes, bool userTyped)
{
    if (userTyped)
        onUserInput();
    if (m_pty)
        m_pty->write(bytes);
}

void TerminalWidget::sendText(const QString &text, bool userTyped)
{
    if (userTyped) {
        onUserInput();
        m_typed += text;
    }
    for (char32_t c : text.toUcs4())
        vterm_keyboard_unichar(m_vt, c, VTERM_MOD_NONE);
    flushOutput();
}

void TerminalWidget::sendKey(VTermKey key, VTermModifier mod)
{
    vterm_keyboard_key(m_vt, key, mod);
    flushOutput();
}

void TerminalWidget::paste(const QString &text)
{
    if (text.isEmpty())
        return;
    onUserInput();
    m_typed += text;
    vterm_keyboard_start_paste(m_vt);
    for (char32_t c : text.toUcs4())
        vterm_keyboard_unichar(m_vt, c == '\n' ? '\r' : c, VTERM_MOD_NONE);
    vterm_keyboard_end_paste(m_vt);
    flushOutput();
}

// ---------------------------------------------------------------------------
// Keyboard

bool TerminalWidget::handleShortcut(QKeyEvent *e, bool dryRun)
{
    auto action = [&](const char *id, auto fn) {
        if (!Shortcuts::instance()->matches(QString::fromUtf8(id), e))
            return false;
        if (!dryRun)
            fn();
        return true;
    };
    if (action("터미널 새 탭", [&] { emit newTabRequested(); })
        || action("터미널 탭 닫기", [&] { emit closeRequested(); }))
        return true;
    // Without a selection, Ctrl+C remains the shell's interrupt on Windows/Linux.
#ifndef Q_OS_MACOS
    if (!selectedText().isEmpty())
#endif
        if (action("터미널 복사", [&] {
                if (!selectedText().isEmpty())
                    QGuiApplication::clipboard()->setText(selectedText());
                m_selStart = m_selEnd = QPoint(-1, -1);
                update();
            }))
            return true;
    if (action("터미널 붙여넣기", [&] { paste(QGuiApplication::clipboard()->text()); })
        || action("터미널 화면 지우기", [&] { m_scrollback.clear(); m_scrollOffset = 0; sendBytes("\x0c", true); })
        || action("터미널 입력줄 지우기", [&] { sendBytes("\x15", true); m_typed.clear(); m_lineRecalled = false; })
        || action("터미널 줄 처음", [&] { sendBytes("\x01", true); })
        || action("터미널 줄 끝", [&] { sendBytes("\x05", true); })
        || action("터미널 이전 단어", [&] { sendBytes("\x1b" "b", true); })
        || action("터미널 다음 단어", [&] { sendBytes("\x1b" "f", true); }))
        return true;
    auto scroll = [&](int by) { m_scrollOffset = qBound(0, m_scrollOffset + by, int(m_scrollback.size())); update(); };
    return action("터미널 이전 페이지", [&] { scroll(m_rows - 1); })
        || action("터미널 다음 페이지", [&] { scroll(1 - m_rows); });
}

bool TerminalWidget::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        // Keep keys meant for the shell away from window shortcuts (Delete would trash files, Space
        // would open Quick Look, ...), but leave ⌘-shortcuts and tab/terminal switching to the app.
        auto *ke = static_cast<QKeyEvent *>(e);
        const Qt::KeyboardModifiers m = ke->modifiers() & ~Qt::KeypadModifier;
        bool mine = handleShortcut(ke, true);
        if (!mine) {
            mine = true;
            for (const QString &id : {QStringLiteral("터미널 펼치기"), QStringLiteral("다음 탭 보기"), QStringLiteral("이전 탭 보기"),
                                     QStringLiteral("설정…"), QStringLiteral("사이드바로 이동"), QStringLiteral("파일뷰로 이동"), QStringLiteral("터미널로 이동")})
                if (Shortcuts::instance()->matches(id, ke))
                    mine = false;
#ifdef Q_OS_MACOS
            if (m & kCmd)
                mine = false;
#endif
        }
        // Pane navigation has priority even when remapped onto a terminal-local key.
        for (const QString &id : {QStringLiteral("사이드바로 이동"), QStringLiteral("파일뷰로 이동"), QStringLiteral("터미널로 이동")})
            if (Shortcuts::instance()->matches(id, ke))
                mine = false;
        if (mine) {
            e->accept();
            return true;
        }
    }
    if (e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) { // not focus navigation
            keyPressEvent(ke);
            return true;
        }
    }
    return QWidget::event(e);
}

void TerminalWidget::keyPressEvent(QKeyEvent *e)
{
    if (handleShortcut(e, false))
        return;
    if (!isRunning()) {
        if (Shortcuts::instance()->matches(QStringLiteral("터미널 다시 시작"), e))
            start(m_cwd.isEmpty() ? m_startCwd : m_cwd);
        return;
    }
    const Qt::KeyboardModifiers m = e->modifiers() & ~Qt::KeypadModifier;
    int vm = VTERM_MOD_NONE;
    if (m & Qt::ShiftModifier)
        vm |= VTERM_MOD_SHIFT;
    if (m & kCtrl)
        vm |= VTERM_MOD_CTRL;
#ifndef Q_OS_MACOS
    if (m & Qt::AltModifier)
        vm |= VTERM_MOD_ALT;
#else
    if ((m & Qt::AltModifier) && e->text().isEmpty())
        vm |= VTERM_MOD_ALT;
#endif
    const auto mod = VTermModifier(vm);

    VTermKey key = VTERM_KEY_NONE;
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter: key = VTERM_KEY_ENTER; break;
    case Qt::Key_Tab: key = VTERM_KEY_TAB; break;
    case Qt::Key_Backtab: key = VTERM_KEY_TAB; break;
    case Qt::Key_Backspace: key = VTERM_KEY_BACKSPACE; break;
    case Qt::Key_Escape: key = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Up: key = VTERM_KEY_UP; break;
    case Qt::Key_Down: key = VTERM_KEY_DOWN; break;
    case Qt::Key_Left: key = VTERM_KEY_LEFT; break;
    case Qt::Key_Right: key = VTERM_KEY_RIGHT; break;
    case Qt::Key_Insert: key = VTERM_KEY_INS; break;
    case Qt::Key_Delete: key = VTERM_KEY_DEL; break;
    case Qt::Key_Home: key = VTERM_KEY_HOME; break;
    case Qt::Key_End: key = VTERM_KEY_END; break;
    case Qt::Key_PageUp: key = VTERM_KEY_PAGEUP; break;
    case Qt::Key_PageDown: key = VTERM_KEY_PAGEDOWN; break;
    default:
        if (e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F12)
            key = VTermKey(VTERM_KEY_FUNCTION(e->key() - Qt::Key_F1 + 1));
    }
    if (key != VTERM_KEY_NONE) {
        if (key == VTERM_KEY_ENTER) {
            sendKey(key, mod);
            lineSubmitted();
            return;
        }
        onUserInput();
        if (key == VTERM_KEY_BACKSPACE)
            m_typed.chop(1);
        if (key == VTERM_KEY_ESCAPE)
            m_typed.clear();
        // History and completion fill the line without typing: until Enter or ⌃C the browser's cd waits.
        if (key == VTERM_KEY_UP || key == VTERM_KEY_DOWN || key == VTERM_KEY_TAB)
            m_lineRecalled = true;
        sendKey(e->key() == Qt::Key_Backtab ? VTERM_KEY_TAB : key,
                e->key() == Qt::Key_Backtab ? VTermModifier(mod | VTERM_MOD_SHIFT) : mod);
        return;
    }
    if (m & kCtrl) {
        // Control characters: ^C, ^D, ^U, ^R, ...
        const int k = e->key();
        char32_t c = 0;
        if (k >= Qt::Key_A && k <= Qt::Key_Z)
            c = char32_t('a' + (k - Qt::Key_A));
        else if (k == Qt::Key_Space || k == Qt::Key_At)
            c = ' ';
        else if (k == Qt::Key_BracketLeft || k == Qt::Key_Backslash || k == Qt::Key_BracketRight || k == Qt::Key_Underscore)
            c = char32_t(k);
        if (c) {
            onUserInput();
            if (c == 'c' || c == 'u' || c == 'd' || c == 'g') {
                m_typed.clear(); // the line is gone
                m_lineRecalled = false;
            } else if (c == 'r' || c == 'p' || c == 'n' || c == 'y') { // history search, previous/next, yank
                m_lineRecalled = true;
            }
            vterm_keyboard_unichar(m_vt, c, VTermModifier(vm & ~VTERM_MOD_SHIFT));
            flushOutput();
            return;
        }
    }
    if (!e->text().isEmpty() && !(m & kCmd))
        sendText(e->text(), true);
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent *e)
{
    if (!e->commitString().isEmpty())
        sendText(e->commitString(), true);
    m_preedit = e->preeditString();
    update();
    e->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery q) const
{
    switch (q) {
    case Qt::ImEnabled: return true;
    case Qt::ImFont: return m_font;
    case Qt::ImCursorRectangle:
        return QRect(kPad + m_cursor.col * m_cellW, kPad + m_cursor.row * m_cellH, m_cellW, m_cellH);
    default: return QWidget::inputMethodQuery(q);
    }
}

void TerminalWidget::focusInEvent(QFocusEvent *e)
{
    QWidget::focusInEvent(e);
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *e)
{
    QWidget::focusOutEvent(e);
    update();
}

// ---------------------------------------------------------------------------
// Screen contents

bool TerminalWidget::cellAt(int line, int col, VTermScreenCell *cell) const
{
    const int sb = int(m_scrollback.size());
    if (line < 0 || col < 0 || col >= m_cols)
        return false;
    if (line < sb) {
        const Line &l = m_scrollback[line];
        if (col >= l.size())
            return false;
        *cell = l[col];
        return true;
    }
    if (line - sb >= m_rows)
        return false;
    return vterm_screen_get_cell(m_screen, VTermPos{line - sb, col}, cell);
}

QPoint TerminalWidget::cellFromPos(const QPoint &pos) const
{
    const int col = qBound(0, (pos.x() - kPad) / m_cellW, m_cols - 1);
    const int row = qBound(0, (pos.y() - kPad) / m_cellH, m_rows - 1);
    return QPoint(col, int(m_scrollback.size()) - m_scrollOffset + row);
}

QString TerminalWidget::textBetween(QPoint a, QPoint b) const
{
    if (b.y() < a.y() || (b.y() == a.y() && b.x() < a.x()))
        std::swap(a, b);
    QStringList lines;
    for (int line = a.y(); line <= b.y(); ++line) {
        QString s;
        const int from = line == a.y() ? a.x() : 0;
        const int to = line == b.y() ? b.x() : m_cols - 1;
        VTermScreenCell cell;
        for (int col = from; col <= to; ++col) {
            if (!cellAt(line, col, &cell))
                break;
            if (isContinuation(cell))
                continue;
            s += cell.chars[0] == 0 ? QStringLiteral(" ") : cellText(cell);
        }
        while (s.endsWith(QLatin1Char(' ')))
            s.chop(1);
        lines << s;
    }
    return lines.join(QLatin1Char('\n'));
}

QString TerminalWidget::selectedText() const
{
    if (m_selStart.y() < 0 || m_selStart == m_selEnd)
        return {};
    return textBetween(m_selStart, m_selEnd);
}

bool TerminalWidget::isSelected(int line, int col) const
{
    if (m_selStart.y() < 0 || m_selStart == m_selEnd)
        return false;
    QPoint a = m_selStart, b = m_selEnd;
    if (b.y() < a.y() || (b.y() == a.y() && b.x() < a.x()))
        std::swap(a, b);
    if (line < a.y() || line > b.y())
        return false;
    if (line == a.y() && col < a.x())
        return false;
    if (line == b.y() && col > b.x())
        return false;
    return true;
}

QString TerminalWidget::screenText() const
{
    const int sb = int(m_scrollback.size());
    return textBetween(QPoint(0, sb), QPoint(m_cols - 1, sb + m_rows - 1));
}

void TerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_bg);
    const QColor selBg = [&] { QColor a = Theme::colors().accent; a.setAlpha(110); return a; }();
    auto color = [this](VTermColor c, bool fg) {
        if (fg && VTERM_COLOR_IS_DEFAULT_FG(&c))
            return m_fg;
        if (!fg && VTERM_COLOR_IS_DEFAULT_BG(&c))
            return m_bg;
        vterm_screen_convert_color_to_rgb(m_screen, &c);
        return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
    };
    const int firstLine = int(m_scrollback.size()) - m_scrollOffset;
    VTermScreenCell cell;
    for (int r = 0; r < m_rows; ++r) {
        const int line = firstLine + r;
        const int y = kPad + r * m_cellH;
        for (int col = 0; col < m_cols; ++col) {
            if (!cellAt(line, col, &cell) || isContinuation(cell))
                continue;
            QColor fg = color(cell.fg, true), bg = color(cell.bg, false);
            if (cell.attrs.reverse)
                std::swap(fg, bg);
            const int x = kPad + col * m_cellW;
            const int w = m_cellW * qMax(1, int(cell.width));
            if (isSelected(line, col))
                bg = selBg;
            if (bg != m_bg)
                p.fillRect(x, y, w, m_cellH, bg);
            if (cell.chars[0] && cell.chars[0] != ' ') {
                p.setFont(cell.attrs.bold ? m_bold : m_font);
                p.setPen(fg);
                p.drawText(x, y + m_ascent, cellText(cell));
            }
            if (cell.attrs.underline)
                p.fillRect(x, y + m_ascent + 2, w, 1, fg);
            if (cell.attrs.strike)
                p.fillRect(x, y + m_cellH / 2, w, 1, fg);
        }
    }
    // Cursor (only at the live screen)
    if (m_scrollOffset == 0 && m_cursorVisible && isRunning()) {
        const QRect cr(kPad + m_cursor.col * m_cellW, kPad + m_cursor.row * m_cellH, m_cellW, m_cellH);
        QColor cc = Theme::colors().accent;
        if (!m_preedit.isEmpty()) {
            const int pw = QFontMetrics(m_font).horizontalAdvance(m_preedit) + 2;
            p.fillRect(QRect(cr.topLeft(), QSize(pw, m_cellH)), m_bg);
            p.setFont(m_font);
            p.setPen(m_fg);
            p.drawText(cr.left(), cr.top() + m_ascent, m_preedit);
            p.fillRect(cr.left(), cr.bottom() - 1, pw, 2, cc);
        } else if (!hasFocus()) {
            p.setPen(QPen(cc, 1));
            p.drawRect(cr.adjusted(0, 0, -1, -1));
        } else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
            p.fillRect(cr.left(), cr.top(), 2, cr.height(), cc);
        } else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
            p.fillRect(cr.left(), cr.bottom() - 1, cr.width(), 2, cc);
        } else {
            cc.setAlpha(200);
            p.fillRect(cr, cc);
            if (cellAt(int(m_scrollback.size()) + m_cursor.row, m_cursor.col, &cell) && !isContinuation(cell) &&
                cell.chars[0] && cell.chars[0] != ' ') {
                p.setFont(m_font);
                p.setPen(m_bg);
                p.drawText(cr.left(), cr.top() + m_ascent, cellText(cell));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Mouse, wheel, drops

void TerminalWidget::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    if (m_mouseMode != VTERM_PROP_MOUSE_NONE && !(e->modifiers() & Qt::ShiftModifier) && m_scrollOffset == 0) {
        const QPoint c = cellFromPos(e->position().toPoint());
        vterm_mouse_move(m_vt, c.y() - int(m_scrollback.size()), c.x(), VTERM_MOD_NONE);
        vterm_mouse_button(m_vt, e->button() == Qt::RightButton ? 3 : e->button() == Qt::MiddleButton ? 2 : 1, true, VTERM_MOD_NONE);
        flushOutput();
        return;
    }
    if (e->button() == Qt::RightButton) {
        QMenu menu(this);
        menu.addAction(Gifiles::tr("복사"), this, [this] { QGuiApplication::clipboard()->setText(selectedText()); })
            ->setEnabled(!selectedText().isEmpty());
        menu.addAction(Gifiles::tr("붙여넣기"), this, [this] { paste(QGuiApplication::clipboard()->text()); });
        menu.addSeparator();
        menu.addAction(Gifiles::tr("지우기"), this, [this] {
            m_scrollback.clear();
            m_scrollOffset = 0;
            sendBytes("\x0c", true);
        });
        menu.exec(e->globalPosition().toPoint());
        return;
    }
    if (e->button() == Qt::LeftButton) {
        m_selStart = m_selEnd = cellFromPos(e->position().toPoint());
        m_selecting = true;
        update();
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_mouseMode >= VTERM_PROP_MOUSE_DRAG && !m_selecting) {
        const QPoint c = cellFromPos(e->position().toPoint());
        vterm_mouse_move(m_vt, c.y() - int(m_scrollback.size()), c.x(), VTERM_MOD_NONE);
        flushOutput();
        return;
    }
    if (m_selecting) {
        m_selEnd = cellFromPos(e->position().toPoint());
        update();
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_mouseMode != VTERM_PROP_MOUSE_NONE && !m_selecting) {
        vterm_mouse_button(m_vt, e->button() == Qt::RightButton ? 3 : e->button() == Qt::MiddleButton ? 2 : 1, false, VTERM_MOD_NONE);
        flushOutput();
        return;
    }
    m_selecting = false;
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Select the word (run of non-space characters) under the pointer.
    const QPoint c = cellFromPos(e->position().toPoint());
    VTermScreenCell cell;
    auto isWord = [&](int col) {
        return cellAt(c.y(), col, &cell) && (isContinuation(cell) || (cell.chars[0] && cell.chars[0] != ' '));
    };
    if (!isWord(c.x()))
        return;
    int a = c.x(), b = c.x();
    while (a > 0 && isWord(a - 1))
        --a;
    while (b < m_cols - 1 && isWord(b + 1))
        ++b;
    m_selStart = QPoint(a, c.y());
    m_selEnd = QPoint(b, c.y());
    m_selecting = false;
    update();
}

void TerminalWidget::wheelEvent(QWheelEvent *e)
{
    // Trackpads report pixels: scroll by those, like native macOS views. Mice report notches.
    const qreal lines = !e->pixelDelta().isNull() ? qreal(e->pixelDelta().y()) / m_cellH
                                                  : e->angleDelta().y() / 120.0 * 3;
    m_wheelAccum += lines;
    const int whole = int(m_wheelAccum);
    m_wheelAccum -= whole;
    if (whole == 0)
        return;
    if (m_altScreen) {
        // Full-screen programs (less, vim): send arrow keys, or wheel buttons if they track the mouse.
        for (int i = 0; i < qAbs(whole); ++i) {
            if (m_mouseMode != VTERM_PROP_MOUSE_NONE) {
                vterm_mouse_button(m_vt, whole > 0 ? 4 : 5, true, VTERM_MOD_NONE);
                vterm_mouse_button(m_vt, whole > 0 ? 4 : 5, false, VTERM_MOD_NONE);
            } else {
                vterm_keyboard_key(m_vt, whole > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
            }
        }
        flushOutput();
        return;
    }
    m_scrollOffset = qBound(0, m_scrollOffset + whole, int(m_scrollback.size()));
    update();
}

void TerminalWidget::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasText()) {
        e->setDropAction(Qt::CopyAction); // never let the source treat this as a move
        e->accept();
    }
}

void TerminalWidget::dropEvent(QDropEvent *e)
{
    QString text;
    if (e->mimeData()->hasUrls()) {
        for (const QUrl &u : e->mimeData()->urls())
            if (u.isLocalFile())
                text += quote(u.toLocalFile()) + QLatin1Char(' ');
    } else {
        text = e->mimeData()->text();
    }
    if ((!m_typed.isEmpty() && !m_typed.endsWith(QLatin1Char(' '))) || (m_typed.isEmpty() && m_lineRecalled))
        text.prepend(QLatin1Char(' '));
    paste(text);
    e->setDropAction(Qt::CopyAction);
    e->accept();
    setFocus();
}
