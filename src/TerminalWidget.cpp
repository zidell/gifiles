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
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>
#include <QtMath>
#include <algorithm>
#include <cmath>

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
// The arms of U+2500–254B, from the center to each edge, as up | right << 2 | down << 4 | left << 6
// (0 none, 1 light, 2 heavy); 0 for the dashed ones (U+2504–250B), which keep the font's glyph.
constexpr uint8_t arms(int up, int right, int down, int left) { return uint8_t(up | right << 2 | down << 4 | left << 6); }
constexpr uint8_t kLines[] = {
    arms(0, 1, 0, 1), arms(0, 2, 0, 2), arms(1, 0, 1, 0), arms(2, 0, 2, 0), 0, 0, 0, 0, 0, 0, 0, 0, // ─━│┃ dashed
    arms(0, 1, 1, 0), arms(0, 2, 1, 0), arms(0, 1, 2, 0), arms(0, 2, 2, 0), // ┌┍┎┏
    arms(0, 0, 1, 1), arms(0, 0, 1, 2), arms(0, 0, 2, 1), arms(0, 0, 2, 2), // ┐┑┒┓
    arms(1, 1, 0, 0), arms(1, 2, 0, 0), arms(2, 1, 0, 0), arms(2, 2, 0, 0), // └┕┖┗
    arms(1, 0, 0, 1), arms(1, 0, 0, 2), arms(2, 0, 0, 1), arms(2, 0, 0, 2), // ┘┙┚┛
    arms(1, 1, 1, 0), arms(1, 2, 1, 0), arms(2, 1, 1, 0), arms(1, 1, 2, 0), // ├┝┞┟
    arms(2, 1, 2, 0), arms(2, 2, 1, 0), arms(1, 2, 2, 0), arms(2, 2, 2, 0), // ┠┡┢┣
    arms(1, 0, 1, 1), arms(1, 0, 1, 2), arms(2, 0, 1, 1), arms(1, 0, 2, 1), // ┤┥┦┧
    arms(2, 0, 2, 1), arms(2, 0, 1, 2), arms(1, 0, 2, 2), arms(2, 0, 2, 2), // ┨┩┪┫
    arms(0, 1, 1, 1), arms(0, 1, 1, 2), arms(0, 2, 1, 1), arms(0, 2, 1, 2), // ┬┭┮┯
    arms(0, 1, 2, 1), arms(0, 1, 2, 2), arms(0, 2, 2, 1), arms(0, 2, 2, 2), // ┰┱┲┳
    arms(1, 1, 0, 1), arms(1, 1, 0, 2), arms(1, 2, 0, 1), arms(1, 2, 0, 2), // ┴┵┶┷
    arms(2, 1, 0, 1), arms(2, 1, 0, 2), arms(2, 2, 0, 1), arms(2, 2, 0, 2), // ┸┹┺┻
    arms(1, 1, 1, 1), arms(1, 1, 1, 2), arms(1, 2, 1, 1), arms(1, 2, 1, 2), // ┼┽┾┿
    arms(2, 1, 1, 1), arms(1, 1, 2, 1), arms(2, 1, 2, 1), arms(2, 1, 1, 2), // ╀╁╂╃
    arms(2, 2, 1, 1), arms(1, 1, 2, 2), arms(1, 2, 2, 1), arms(2, 2, 1, 2), // ╄╅╆╇
    arms(1, 2, 2, 2), arms(2, 1, 2, 2), arms(2, 2, 2, 1), arms(2, 2, 2, 2), // ╈╉╊╋
};
// U+2574–257F: half lines.
constexpr uint8_t kHalfLines[] = {
    arms(0, 0, 0, 1), arms(1, 0, 0, 0), arms(0, 1, 0, 0), arms(0, 0, 1, 0), // ╴╵╶╷
    arms(0, 0, 0, 2), arms(2, 0, 0, 0), arms(0, 2, 0, 0), arms(0, 0, 2, 0), // ╸╹╺╻
    arms(0, 2, 0, 1), arms(1, 0, 2, 0), arms(0, 1, 0, 2), arms(2, 0, 1, 0), // ╼╽╾╿
};
// U+2596–259F: quadrants as upper-left 1 | upper-right 2 | lower-left 4 | lower-right 8.
constexpr uint8_t kQuadrants[] = {4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8};

double luminance(const QColor &c)
{
    auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}
} // namespace

int TerminalWidget::incompleteUtf8Tail(const QByteArray &data)
{
    const int n = int(data.size());
    int i = n - 1;
    while (i >= 0 && i >= n - 4 && (uchar(data[i]) & 0xC0) == 0x80) // continuation bytes back to the lead byte
        --i;
    if (i < 0 || i < n - 4)
        return 0;
    const uchar lead = uchar(data[i]);
    const int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return n - i < need ? n - i : 0;
}

QColor TerminalWidget::readable(const QColor &fg, const QColor &bg, double ratio)
{
    const double lb = luminance(bg);
    auto contrast = [lb](const QColor &c) {
        const double l = luminance(c);
        return (qMax(l, lb) + 0.05) / (qMin(l, lb) + 0.05);
    };
    if (contrast(fg) >= ratio)
        return fg;
    const QColor target = lb < 0.18 ? Qt::white : Qt::black;
    auto mix = [&](double t) {
        return QColor::fromRgbF(float(fg.redF() + (target.redF() - fg.redF()) * t), float(fg.greenF() + (target.greenF() - fg.greenF()) * t),
                                float(fg.blueF() + (target.blueF() - fg.blueF()) * t));
    };
    double lo = 0, hi = 1;
    for (int k = 0; k < 12; ++k) { // the least mix that reaches the ratio
        const double mid = (lo + hi) / 2;
        (contrast(mix(mid)) < ratio ? lo : hi) = mid;
    }
    return mix(hi);
}

bool TerminalWidget::drawBoxGlyph(QPainter &p, const QRectF &cell, char32_t ch, const QColor &color)
{
    const qreal x = cell.x(), y = cell.y(), w = cell.width(), h = cell.height();
    const qreal light = qMax<qreal>(1, std::round(w / 8)), heavy = 2 * light;
    // Line centers on whole pixels, so neighbouring cells' lines meet exactly.
    const qreal cx = x + std::floor(w / 2), cy = y + std::floor(h / 2);
    uint8_t a = 0;
    if (ch >= 0x2500 && ch <= 0x254B)
        a = kLines[ch - 0x2500];
    else if (ch >= 0x2574 && ch <= 0x257F)
        a = kHalfLines[ch - 0x2574];
    if (a) {
        auto thick = [&](int shift) { const int k = (a >> shift) & 3; return k == 0 ? 0.0 : k == 1 ? light : heavy; };
        const qreal up = thick(0), right = thick(2), down = thick(4), left = thick(6);
        // Each arm runs from its edge into the center far enough to cover the crossing arms.
        const qreal hMax = qMax(left, right), vMax = qMax(up, down);
        if (up)
            p.fillRect(QRectF(cx - up / 2, y, up, cy - y + qMax(hMax, up) / 2), color);
        if (down)
            p.fillRect(QRectF(cx - down / 2, cy - qMax(hMax, down) / 2, down, y + h - cy + qMax(hMax, down) / 2), color);
        if (left)
            p.fillRect(QRectF(x, cy - left / 2, cx - x + qMax(vMax, left) / 2, left), color);
        if (right)
            p.fillRect(QRectF(cx - qMax(vMax, right) / 2, cy - right / 2, x + w - cx + qMax(vMax, right) / 2, right), color);
        return true;
    }
    if (ch >= 0x256D && ch <= 0x2570) { // ╭╮╯╰
        const bool goesDown = ch == 0x256D || ch == 0x256E, goesRight = ch == 0x256D || ch == 0x2570;
        const qreal r = qMin(w, h) / 2;
        const qreal vy = goesDown ? y + h : y, hx = goesRight ? x + w : x;
        const qreal ry = goesDown ? cy + r : cy - r, rx = goesRight ? cx + r : cx - r;
        QPainterPath path(QPointF(cx, vy));
        path.lineTo(cx, ry);
        path.quadTo(cx, cy, rx, cy);
        path.lineTo(hx, cy);
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, light, Qt::SolidLine, Qt::FlatCap));
        p.drawPath(path); // centered on cx/cy, like the straight lines
        p.restore();
        return true;
    }
    if (ch >= 0x2580 && ch <= 0x259F) {
        auto eighths = [&](int n) { return std::round(h * n / 8); };
        auto columns = [&](int n) { return std::round(w * n / 8); };
        if (ch == 0x2580) // ▀
            p.fillRect(QRectF(x, y, w, eighths(4)), color);
        else if (ch <= 0x2588) // ▁…█
            p.fillRect(QRectF(x, y + h - eighths(int(ch - 0x2580)), w, eighths(int(ch - 0x2580))), color);
        else if (ch <= 0x258F) // ▉…▏
            p.fillRect(QRectF(x, y, columns(int(0x2590 - ch)), h), color);
        else if (ch == 0x2590) // ▐
            p.fillRect(QRectF(x + columns(4), y, w - columns(4), h), color);
        else if (ch <= 0x2593) { // ░▒▓
            QColor c = color;
            c.setAlphaF(color.alphaF() * (ch - 0x2590) / 4.0);
            p.fillRect(cell, c);
        } else if (ch == 0x2594) // ▔
            p.fillRect(QRectF(x, y, w, eighths(1)), color);
        else if (ch == 0x2595) // ▕
            p.fillRect(QRectF(x + w - columns(1), y, columns(1), h), color);
        else {
            const uint8_t q = kQuadrants[ch - 0x2596];
            const qreal mx = columns(4), my = eighths(4);
            if (q & 1)
                p.fillRect(QRectF(x, y, mx, my), color);
            if (q & 2)
                p.fillRect(QRectF(x + mx, y, w - mx, my), color);
            if (q & 4)
                p.fillRect(QRectF(x, y + my, mx, h - my), color);
            if (q & 8)
                p.fillRect(QRectF(x + mx, y + my, w - mx, h - my), color);
        }
        return true;
    }
    return false;
}

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
        if (visible)
            w(u)->m_markedAt = pos;
        w(u)->update();
        return 1;
    }
    static int settermprop(VTermProp prop, VTermValue *val, void *u)
    {
        TerminalWidget *t = w(u);
        switch (prop) {
        case VTERM_PROP_CURSORVISIBLE:
            t->m_cursorVisible = val->boolean;
            if (t->m_cursorVisible)
                t->m_markedAt = t->m_cursor;
            break;
        case VTERM_PROP_CURSORSHAPE: t->m_cursorShape = val->number; break;
        case VTERM_PROP_ALTSCREEN: t->m_altScreen = val->boolean; t->m_scrollOffset = 0; break;
        case VTERM_PROP_MOUSE: t->m_mouseMode = val->number; break;
        case VTERM_PROP_TITLE: {
            QByteArray &buf = t->m_titleBuf;
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
        if (t->m_scrollOffset > 0) // keep the history view still while output arrives (a full history too)
            t->m_scrollOffset = qMin(t->m_scrollOffset + 1, int(t->m_scrollback.size()));
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
        // OSC 10/11 "?": programs (lipgloss, vim) ask for the default text/background color to pick dark or
        // light colors. libvterm doesn't answer; without one they assume dark (Terminal.app answers).
        if ((command == 10 || command == 11) && frag.initial && frag.final && frag.len == 1 && frag.str[0] == '?') {
            const QColor c = command == 10 ? w(u)->m_fg : w(u)->m_bg;
            w(u)->m_outBuf += QStringLiteral("\x1b]%1;rgb:%2/%3/%4\x1b\\")
                                  .arg(command)
                                  .arg(c.red() * 257, 4, 16, QLatin1Char('0'))
                                  .arg(c.green() * 257, 4, 16, QLatin1Char('0'))
                                  .arg(c.blue() * 257, 4, 16, QLatin1Char('0'))
                                  .toLatin1();
            return 1;
        }
        if (command != 7)
            return 0;
        QByteArray &buf = w(u)->m_oscBuf;
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
        if (key == QLatin1String(Settings::TermFontFamily) || key == QLatin1String(Settings::TermFontSize)
            || key == QLatin1String(Settings::TermLineHeight))
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
    m_readable.clear();
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

QString TerminalWidget::bundledFont()
{
    // D2Coding (SIL OFL 1.1) is embedded as a Qt resource and registered for this process the first time a
    // terminal needs it, so a session that never opens the terminal never loads its 8 MB. Empty if the
    // platform refuses it (the terminal then uses the system's fixed-width font).
    static const QString family = [] {
        QString name;
        for (const char *file : {":/fonts/D2Coding.ttf", ":/fonts/D2CodingBold.ttf"}) {
            const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(file));
            if (id >= 0 && name.isEmpty())
                name = QFontDatabase::applicationFontFamilies(id).value(0);
        }
        return name;
    }();
    return family;
}

void TerminalWidget::updateFont()
{
    QString family = Settings::instance()->value(Settings::TermFontFamily).toString();
    if (family.isEmpty())
        family = bundledFont();
    m_font = family.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : QFont(family);
    m_font.setPointSize(Settings::instance()->value(Settings::TermFontSize).toInt());
    m_font.setStyleHint(QFont::Monospace);
    m_bold = m_font;
    m_bold.setBold(true);
    const QFontMetricsF fm(m_font);
    m_cellW = qCeil(fm.horizontalAdvance(QLatin1Char('M')));
    // Line spacing (terminal.line_height, %) stretches the cell; the extra is split above and below the text.
    const int natural = qCeil(fm.height()) + 2;
    m_cellH = qMax(natural, qCeil(natural * Settings::instance()->value(Settings::TermLineHeight).toInt() / 100.0));
    m_ascent = qCeil(fm.ascent()) + 1 + (m_cellH - natural) / 2;
    m_wideScale.clear();
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
    connect(m_pty, &Pty::dataReceived, this, [this](const QByteArray &chunk) {
        // libvterm 0.3.3 turns a multi-byte character split across two writes into U+FFFD when an escape
        // sequence came before it (a colored "←↑ 이동" cut anywhere inside a character, measured), so the
        // unfinished character at the end waits for the next read.
        const QByteArray data = m_utf8Carry + chunk;
        const int keep = incompleteUtf8Tail(data);
        m_utf8Carry = data.right(keep);
        vterm_input_write(m_vt, data.constData(), size_t(data.size() - keep));
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
        // The cursor moves inside the line: m_typed no longer tells what is on it (see keyPressEvent).
        || action("터미널 줄 처음", [&] { sendBytes("\x01", true); m_lineRecalled = true; })
        || action("터미널 줄 끝", [&] { sendBytes("\x05", true); m_lineRecalled = true; })
        || action("터미널 이전 단어", [&] { sendBytes("\x1b" "b", true); m_lineRecalled = true; })
        || action("터미널 다음 단어", [&] { sendBytes("\x1b" "f", true); m_lineRecalled = true; }))
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
            const bool command = !isBusy() && (!m_typed.trimmed().isEmpty() || m_lineRecalled);
            const QString line = m_typed;
            sendKey(key, mod);
            lineSubmitted();
            if (command)
                emit commandEntered(line);
            return;
        }
        onUserInput();
        if (key == VTERM_KEY_BACKSPACE)
            m_typed.chop(1);
        if (key == VTERM_KEY_ESCAPE)
            m_typed.clear();
        // History and completion fill the line without typing, and once the cursor moves inside the
        // line what is typed or erased no longer adds up to m_typed: until Enter or ⌃C the line is
        // unknown and the browser's cd (which clears the line first) waits.
        if (key == VTERM_KEY_UP || key == VTERM_KEY_DOWN || key == VTERM_KEY_TAB || key == VTERM_KEY_LEFT ||
            key == VTERM_KEY_RIGHT || key == VTERM_KEY_HOME || key == VTERM_KEY_END || key == VTERM_KEY_DEL)
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
            } else if (c == 'r' || c == 'p' || c == 'n' || c == 'y' || // history search, previous/next, yank
                       c == 'a' || c == 'e' || c == 'b' || c == 'f' || c == 't') { // the cursor moves: as above
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
    // A click already sent the composing text (commitPreedit); the IME's own commit of it comes after (async for
    // Gureum's commitComposition) and is dropped.
    const bool clickCommitted = e->commitString() == m_committedByClick && m_committedAt.isValid() && m_committedAt.elapsed() < 500;
    if (!e->commitString().isEmpty() && !clickCommitted)
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
        return QRect(kPad + m_markedAt.col * m_cellW, kPad + m_markedAt.row * m_cellH, m_cellW, m_cellH);
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
    const int row = qBound(0, (pos.y() - kPad) / m_cellH, m_rows - 1);
    const int line = int(m_scrollback.size()) - m_scrollOffset + row;
    int col = (pos.x() - kPad) / m_cellW;
    if (const VTermLineInfo *info = lineInfo(line); info && info->doublewidth)
        col /= 2; // drawn twice as wide
    return QPoint(qBound(0, col, m_cols - 1), line);
}

const VTermLineInfo *TerminalWidget::lineInfo(int line) const
{
    const int row = line - int(m_scrollback.size());
    return row >= 0 && row < m_rows ? vterm_state_get_lineinfo(vterm_obtain_state(m_vt), row) : nullptr;
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
        // DEC double-width (DECDWL) and double-height (DECDHL) lines, which TUIs use for big titles: the cells are
        // drawn twice as large; a double-height pair draws the top and the bottom half of the same text.
        const VTermLineInfo *info = lineInfo(line);
        const bool dwl = info && info->doublewidth;
        if (dwl) {
            p.save();
            p.setClipRect(0, y, width(), m_cellH);
            p.translate(kPad, info->doubleheight == 2 ? y - m_cellH : y);
            p.scale(2, info->doubleheight ? 2 : 1);
            p.translate(-kPad, -y);
        }
        for (int col = 0; col < (dwl ? m_cols / 2 : m_cols); ++col) {
            if (!cellAt(line, col, &cell) || isContinuation(cell))
                continue;
            QColor fg = color(cell.fg, true), bg = color(cell.bg, false);
            if (cell.attrs.reverse)
                std::swap(fg, bg);
#ifdef VTERM_HAS_DIM
            if (cell.attrs.dim) // SGR 2 (faint, e.g. Claude Code's suggestions): halfway to the background
                fg = QColor((fg.red() + bg.red()) / 2, (fg.green() + bg.green()) / 2, (fg.blue() + bg.blue()) / 2);
#endif
            // Text too close to its background is moved until readable, like Terminal.app does in dark mode
            // (256-color 235 #262626 drawn as #717171, measured): TUIs pick such faint colors for their lines.
            const quint64 key = quint64(fg.rgb() & 0xFFFFFF) << 24 | (bg.rgb() & 0xFFFFFF);
            auto it = m_readable.constFind(key);
            if (it == m_readable.cend())
                it = m_readable.insert(key, readable(fg, bg, 3.0).rgb());
            fg = QColor::fromRgb(*it);
            const int x = kPad + col * m_cellW;
            const int w = m_cellW * qMax(1, int(cell.width));
            if (isSelected(line, col))
                bg = selBg;
            if (bg != m_bg)
                p.fillRect(x, y, w, m_cellH, bg);
            if (cell.chars[0] && cell.chars[0] != ' ') {
                if (cell.chars[1] || !drawBoxGlyph(p, QRectF(x, y, w, m_cellH), cell.chars[0], fg)) {
                    QFont f = cell.attrs.bold ? m_bold : m_font;
                    const QString text = cellText(cell);
                    int tx = x;
                    if (cell.width > 1) {
                        // A 2-cell character (Hangul, CJK) from a fallback font is narrower than its two cells,
                        // leaving gaps inside words; enlarge it to fill them like Terminal.app (within the line).
                        auto it = m_wideScale.constFind(cell.chars[0]);
                        if (it == m_wideScale.cend()) {
                            const QFontMetricsF fm(f);
                            const qreal adv = fm.horizontalAdvance(text);
                            const qreal s = adv > 0 && adv < w * 0.9 ? std::min({w * 0.95 / adv, 1.4, m_cellH / fm.height()}) : 1.0;
                            it = m_wideScale.insert(cell.chars[0], qMax(1.0, s));
                        }
                        if (*it > 1.0)
                            f.setPointSizeF(f.pointSizeF() * *it);
                        tx = x + qMax(0, int((w - QFontMetricsF(f).horizontalAdvance(text)) / 2));
                    }
                    p.setFont(f);
                    p.setPen(fg);
                    p.drawText(tx, y + m_ascent, text);
                }
            }
            if (cell.attrs.underline)
                p.fillRect(x, y + m_ascent + 2, w, 1, fg);
            if (cell.attrs.strike)
                p.fillRect(x, y + m_cellH / 2, w, 1, fg);
        }
        if (dwl)
            p.restore();
    }
    // Cursor (only at the live screen)
    // Composing text, at the cursor's last shown position (m_markedAt), even while a program hides the cursor.
    if (m_scrollOffset == 0 && isRunning() && !m_preedit.isEmpty()) {
        const QRect cr(kPad + m_markedAt.col * m_cellW, kPad + m_markedAt.row * m_cellH, m_cellW, m_cellH);
        const int pw = QFontMetrics(m_font).horizontalAdvance(m_preedit) + 2;
        p.fillRect(QRect(cr.topLeft(), QSize(pw, m_cellH)), m_bg);
        p.setFont(m_font);
        p.setPen(m_fg);
        p.drawText(cr.left(), cr.top() + m_ascent, m_preedit);
        p.fillRect(cr.left(), cr.bottom() - 1, pw, 2, Theme::colors().accent);
    } else if (m_scrollOffset == 0 && m_cursorVisible && isRunning()) {
        const QRect cr(kPad + m_cursor.col * m_cellW, kPad + m_cursor.row * m_cellH, m_cellW, m_cellH);
        QColor cc = Theme::colors().accent;
        if (!hasFocus()) {
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
                if (cell.chars[1] || !drawBoxGlyph(p, cr, cell.chars[0], m_bg)) {
                    p.setFont(m_font);
                    p.setPen(m_bg);
                    p.drawText(cr.left(), cr.top() + m_ascent, cellText(cell));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Mouse, wheel, drops

// Modifiers for the mouse report. It has no bit for ⌘, so ⌘-click goes as Ctrl-click (a toggle in lists).
int TerminalWidget::mouseMods(Qt::KeyboardModifiers m) const
{
    int vm = VTERM_MOD_NONE;
    if (m & Qt::ShiftModifier)
        vm |= VTERM_MOD_SHIFT;
    if (m & Qt::AltModifier)
        vm |= VTERM_MOD_ALT;
    if (m & (kCtrl | kCmd))
        vm |= VTERM_MOD_CTRL;
    return vm;
}

void TerminalWidget::sendMouse(QPoint cell, int button, bool pressed, int mods)
{
    vterm_mouse_move(m_vt, cell.y() - int(m_scrollback.size()), cell.x(), VTermModifier(mods));
    vterm_mouse_button(m_vt, button, pressed, VTermModifier(mods));
    flushOutput();
}

// A click commits the composing text, as Terminal.app does; otherwise it would stay on screen while the program
// moves on. It is sent here at once: the IME's commit comes later (async) and could land on another screen.
void TerminalWidget::commitPreedit()
{
    if (m_preedit.isEmpty())
        return;
    const QString text = m_preedit;
    m_preedit.clear();
    sendText(text, true);
    m_committedByClick = text;
    m_committedAt.start();
    QGuiApplication::inputMethod()->reset();
    update();
}

void TerminalWidget::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    commitPreedit();
    const int button = e->button() == Qt::RightButton ? 3 : e->button() == Qt::MiddleButton ? 2 : 1;
    // Shift-right-click keeps the menu below (복사·붙여넣기) over a program that takes the mouse.
    if (m_mouseMode != VTERM_PROP_MOUSE_NONE && m_scrollOffset == 0 && !(button == 3 && (e->modifiers() & Qt::ShiftModifier))) {
        const QPoint c = cellFromPos(e->position().toPoint());
        if (button == 1 && !(e->modifiers() & Qt::AltModifier)) {
            m_pendingPress = true;
            m_pressCell = c;
            m_pressMods = mouseMods(e->modifiers());
            return;
        }
        m_buttonToProgram = button;
        sendMouse(c, button, true, mouseMods(e->modifiers()));
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
    const QPoint c = cellFromPos(e->position().toPoint());
    if (m_buttonToProgram) {
        if (m_mouseMode >= VTERM_PROP_MOUSE_DRAG) {
            vterm_mouse_move(m_vt, c.y() - int(m_scrollback.size()), c.x(), VTermModifier(mouseMods(e->modifiers())));
            flushOutput();
        }
        return;
    }
    if (m_pendingPress) {
        if (c == m_pressCell)
            return;
        m_pendingPress = false; // dragged off the cell: a selection, not a click
        m_selStart = m_pressCell;
        m_selecting = true;
    }
    if (m_selecting) {
        m_selEnd = cellFromPos(e->position().toPoint());
        update();
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_buttonToProgram) {
        sendMouse(cellFromPos(e->position().toPoint()), m_buttonToProgram, false, mouseMods(e->modifiers()));
        m_buttonToProgram = 0;
        return;
    }
    if (m_pendingPress) {
        m_pendingPress = false;
        sendMouse(m_pressCell, 1, true, m_pressMods);
        sendMouse(m_pressCell, 1, false, m_pressMods);
        return;
    }
    m_selecting = false;
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    // A program that takes the mouse gets the second click as a click (Qt sends no press for it).
    if (m_mouseMode != VTERM_PROP_MOUSE_NONE && m_scrollOffset == 0)
        return mousePressEvent(e);
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
