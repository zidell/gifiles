#include "Log.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMessageBox>
#include <QMutex>
#include <QPlainTextEdit>
#include <QTimer>

namespace Log {
namespace {

QString g_dir;
QFile g_file;
QDate g_day;
QMutex g_mutex;
QtMessageHandler g_previous = nullptr;

QString g_recentKey;
QElapsedTimer g_recentKeyAt;

void open(const QDate &day)
{
    g_file.close();
    g_file.setFileName(QDir(g_dir).filePath(QStringLiteral("gifiles-%1.log").arg(day.toString(Qt::ISODate))));
    g_file.open(QIODevice::Append | QIODevice::Text);
    g_day = day;
}

void messageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (type != QtDebugMsg && type != QtInfoMsg)
        write(type == QtWarningMsg ? "qt-warning" : "qt-error", msg);
    if (g_previous)
        g_previous(type, ctx, msg);
}

QString widgetName(QObject *o)
{
    if (!o)
        return QStringLiteral("-");
    QString n = QString::fromLatin1(o->metaObject()->className());
    if (!o->objectName().isEmpty())
        n += QLatin1Char('#') + o->objectName();
    return n;
}

// Typing into a name, the search box or the shell is counted, not recorded. In the file views a
// plain key is a command (Space, Return, type-to-select) and is recorded.
bool isTyping(QObject *obj, const QKeyEvent *ke)
{
    if (!qobject_cast<QLineEdit *>(obj) && !qobject_cast<QPlainTextEdit *>(obj) && !obj->inherits("TerminalWidget"))
        return false;
    const Qt::KeyboardModifiers m = ke->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);
    return m == Qt::NoModifier && !ke->text().isEmpty() && ke->text().at(0).isPrint();
}

class Watcher : public QObject {
public:
    Watcher()
    {
        m_flush.setSingleShot(true);
        m_flush.setInterval(1500);
        connect(&m_flush, &QTimer::timeout, this, [this] { flushTyping(); });
    }

    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        switch (ev->type()) {
        case QEvent::ShortcutOverride: {
            // Comes before the key press (and before any shortcut), once per key, to the focus widget.
            auto *ke = static_cast<QKeyEvent *>(ev);
            if (obj != QApplication::focusWidget() || ke->isAutoRepeat() || ke->timestamp() == m_lastStamp)
                break;
            m_lastStamp = ke->timestamp();
            const int k = ke->key();
            if (k == Qt::Key_Shift || k == Qt::Key_Control || k == Qt::Key_Meta || k == Qt::Key_Alt || k == 0 ||
                k == Qt::Key_unknown)
                break;
            if (isTyping(obj, ke)) {
                if (m_typedIn != obj)
                    flushTyping();
                m_typedIn = obj;
                ++m_typed;
                m_flush.start();
                break;
            }
            flushTyping();
            const QKeySequence seq(QKeyCombination(ke->modifiers() & ~Qt::KeypadModifier, Qt::Key(k)));
            g_recentKey = seq.toString(QKeySequence::NativeText);
            g_recentKeyAt.start();
            write("key", QStringLiteral("%1  (%2)").arg(g_recentKey, widgetName(obj)));
            break;
        }
        case QEvent::MouseButtonDblClick:
            if (qobject_cast<QAbstractItemView *>(obj ? obj->parent() : nullptr))
                write("mouse", QStringLiteral("double-click (%1)").arg(widgetName(obj->parent())));
            break;
        case QEvent::Show:
            if (auto *box = qobject_cast<QMessageBox *>(obj))
                write("dialog", QStringLiteral("%1 | %2 | %3").arg(box->windowTitle(), box->text(), box->informativeText()));
            break;
        default:
            break;
        }
        return false;
    }

private:
    void flushTyping()
    {
        if (m_typed > 0)
            write("typing", QStringLiteral("%1 characters (%2)").arg(m_typed).arg(widgetName(m_typedIn)));
        m_typed = 0;
        m_typedIn = nullptr;
    }

    ulong m_lastStamp = 0;
    int m_typed = 0;
    QObject *m_typedIn = nullptr;
    QTimer m_flush;
};

} // namespace

void start(const QString &dir, int keepDays)
{
    if (!QDir().mkpath(dir))
        return;
    g_dir = dir;
    // Keep a week of files.
    const QDate oldest = QDate::currentDate().addDays(-keepDays);
    for (const QFileInfo &fi : QDir(dir).entryInfoList({QStringLiteral("gifiles-*.log")}, QDir::Files))
        if (const QDate d = QDate::fromString(fi.completeBaseName().mid(8), Qt::ISODate); d.isValid() && d < oldest)
            QFile::remove(fi.absoluteFilePath());
    open(QDate::currentDate());
    g_previous = qInstallMessageHandler(messageHandler);
    if (qApp)
        qApp->installEventFilter(new Watcher);
}

bool enabled() { return !g_dir.isEmpty(); }

void write(const char *category, const QString &text)
{
    if (g_dir.isEmpty())
        return;
    QMutexLocker lock(&g_mutex); // file jobs and Qt warnings may come from worker threads
    const QDateTime now = QDateTime::currentDateTime();
    if (now.date() != g_day)
        open(now.date());
    const QByteArray line = QStringLiteral("%1 [%2] %3\n")
                                .arg(now.toString(QStringLiteral("HH:mm:ss.zzz")), QString::fromLatin1(category), text)
                                .toUtf8();
    g_file.write(line);
    g_file.flush();
}

QString recentKey()
{
    return g_recentKeyAt.isValid() && g_recentKeyAt.elapsed() < 100 ? g_recentKey : QString();
}

} // namespace Log
