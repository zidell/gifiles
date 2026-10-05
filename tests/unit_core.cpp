// Unit tests of the core logic that needs no window: the TOML subset (Toml), config.toml's schema,
// checks and rendering (Settings), shell quoting of user commands (TerminalWidget::quoteWord /
// expandCommand), the pure helpers (Util), file name colors (Theme), shortcut ids (Shortcuts),
// natural sorting (FileProxy) and the command-line options of the real executable (main.cpp).
//
// Every test runs untranslated (Korean source text) with GIFILES_CONFIG_DIR in a temporary folder:
// the user's own config.toml is never read or written.

#include "Headless.h"

#include "FileProxy.h"
#include "FolderIndex.h"
#include "FolderTree.h"
#include "RecentFolders.h"
#include "FileOps.h"
#include "Settings.h"
#include "Shortcuts.h"
#include "TerminalWidget.h"
#include "Theme.h"
#include "Toml.h"
#include "Util.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileSystemModel>
#include <QImage>
#include <QKeySequence>
#include <QLocale>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

#include <cmath>

namespace {

// GIFILES_CONFIG_DIR pointed elsewhere for the static functions (renderEffective, uiLanguage),
// restored on scope exit so the Settings instance keeps its own folder.
class ConfigDirOverride {
public:
    explicit ConfigDirOverride(const QString &dir) : m_old(qgetenv("GIFILES_CONFIG_DIR"))
    {
        qputenv("GIFILES_CONFIG_DIR", QFile::encodeName(dir));
    }
    ~ConfigDirOverride() { qputenv("GIFILES_CONFIG_DIR", m_old); }

private:
    QByteArray m_old;
};

bool writeText(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray bytes = text.toUtf8();
    return f.write(bytes) == bytes.size();
}

QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

bool parseOk(const QString &text, toml::Document *doc = nullptr, toml::Error *error = nullptr)
{
    toml::Document d;
    toml::Error e;
    const bool ok = toml::parse(text, d, e);
    if (doc)
        *doc = d;
    if (error)
        *error = e;
    return ok;
}

// OKLab lightness / hue of an sRGB color (Björn Ottosson), to check Theme::lightModeColor.
struct Lab {
    double l, a, b;
};
Lab oklab(const QColor &c)
{
    auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    const double r = lin(c.redF()), g = lin(c.greenF()), b = lin(c.blueF());
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

// Names that try to break out of shell quoting.
QStringList nastyWords()
{
    return {
        QStringLiteral("plain"),
        QStringLiteral("with space"),
        QStringLiteral("it's"),
        QStringLiteral("'"),
        QStringLiteral("\"double\""),
        QStringLiteral("a'b\"c"),
        QStringLiteral("$HOME"),
        QStringLiteral("$(touch PWNED1)"),
        QStringLiteral("`touch PWNED2`"),
        QStringLiteral("${IFS}x"),
        QStringLiteral("back\\slash"),
        QStringLiteral("trailing\\"),
        QStringLiteral("\\'"),
        QStringLiteral("semi;touch PWNED3"),
        QStringLiteral("pipe|and&amp"),
        QStringLiteral("glob*?[a]"),
        QStringLiteral("~tilde"),
        QStringLiteral("!bang"),
        QStringLiteral("{a,b}"),
        QStringLiteral("%s%n"),
        QStringLiteral("-n"),
        QStringLiteral("--"),
        QStringLiteral("-rf"),
        QStringLiteral("line1\nline2"),
        QStringLiteral("cr\rlf"),
        QStringLiteral("tab\there"),
        QStringLiteral("bell\x07" "esc\x1b[31m"),
        QStringLiteral("\x01" "2"), // \x01 followed by a hex digit
        QStringLiteral("del\x7f"),
        QStringLiteral("한글 파일.txt"),
        QString::fromUtf8("\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab"), // NFD 한
        QString::fromUtf8("emoji \xf0\x9f\x98\x80"),
        QString::fromUtf8("\xe2\x80\x9csmart\xe2\x80\x9d \xe2\x80\x9e \xe2\x80\x98single\xe2\x80\x99"),
        QStringLiteral(" leading and trailing "),
        QString(),
    };
}

#ifndef Q_OS_WIN
// Runs `script` in `shell`, given on stdin (QProcess arguments would be re-encoded: NFD on macOS).
QByteArray runShell(const QString &shell, const QString &script, const QString &cwd, int *exitCode)
{
    QProcess p;
    p.setWorkingDirectory(cwd);
    p.start(shell, {QStringLiteral("-s")});
    p.write(script.toUtf8() + '\n');
    p.closeWriteChannel();
    if (!p.waitForFinished(10000)) {
        p.kill();
        p.waitForFinished();
        *exitCode = -1;
        return "<timeout>";
    }
    *exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    return p.readAllStandardOutput();
}

// The same, stdout split at NUL bytes (printf '%s\0').
QStringList runShellWords(const QString &shell, const QString &script, const QString &cwd, int *exitCode)
{
    const QByteArray bytes = runShell(shell, script, cwd, exitCode);
    QStringList out;
    for (const QByteArray &w : bytes.split('\0'))
        out << QString::fromUtf8(w);
    if (!out.isEmpty() && out.last().isEmpty() && bytes.endsWith('\0'))
        out.removeLast();
    return out;
}
#else
// Runs a PowerShell script (-EncodedCommand, as MainWindow::runQuietly does) and returns its stdout lines.
QStringList runPowerShell(const QString &script)
{
    QByteArray utf16;
    for (const QChar c : script) {
        utf16.append(char(c.unicode() & 0xff));
        utf16.append(char(c.unicode() >> 8));
    }
    QProcess p;
    p.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                                               QStringLiteral("-EncodedCommand"), QString::fromLatin1(utf16.toBase64())});
    if (!p.waitForFinished(30000))
        return {QStringLiteral("<timeout>")};
    QStringList lines = QString::fromLatin1(p.readAllStandardOutput()).split(QStringLiteral("\r\n"));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

// One line per word: its UTF-16 units as hex, so the console code page can't mangle anything.
QString hexUnits(const QString &s)
{
    QStringList units;
    for (const QChar c : s)
        units << QStringLiteral("%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
    return units.join(QLatin1Char(' '));
}
#endif

} // namespace

class Unit : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;  // everything the tests write
    QString m_configDir;   // the Settings instance's GIFILES_CONFIG_DIR

    QString scratch(const QString &name)
    {
        const QString p = m_root.filePath(name);
        QDir(p).removeRecursively();
        QDir().mkpath(p);
        return p;
    }

    // Writes config.toml behind the Settings instance's back, as an editor would.
    void editConfig(const QString &text) { QVERIFY(writeText(Settings::configPath(), text)); }

    struct Run {
        int code = -1;
        QString out, err;
    };
    Run runApp(const QStringList &args, const QString &configDir)
    {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GIFILES_CONFIG_DIR"), configDir);
        env.remove(QStringLiteral("GIFILES_SNAPSHOT"));
        p.setProcessEnvironment(env);
        p.start(QStringLiteral(GIFILES_APP_EXE), args);
        Run r;
        if (!p.waitForFinished(20000)) {
            p.kill();
            p.waitForFinished();
            r.err = QStringLiteral("<timeout>");
            return r;
        }
        r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
        r.out = QString::fromUtf8(p.readAllStandardOutput());
        // Qt's own log lines (Linux: "qt.multimedia.symbolsresolver: Couldn't load pipewire") aren't the app's output.
        QStringList errLines;
        for (const QString &l : QString::fromUtf8(p.readAllStandardError()).split(QLatin1Char('\n')))
            if (!l.startsWith(QLatin1String("qt.")))
                errLines << l;
        r.err = errLines.join(QLatin1Char('\n'));
        return r;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        m_configDir = m_root.filePath(QStringLiteral("config"));
        qputenv("GIFILES_CONFIG_DIR", QFile::encodeName(m_configDir));
        QCoreApplication::setOrganizationName(QStringLiteral("gifiles-unit-core")); // QSettings state of its own
        QSettings().clear();
        QVERIFY(QFileInfo::exists(QStringLiteral(GIFILES_APP_EXE)));
    }

    // ---------------------------------------------------------------- TOML

    void tomlScalars()
    {
        toml::Document d;
        QVERIFY(parseOk(QStringLiteral("top = 1\n"
                                       "[t]\n"
                                       "yes = true\nno = false\n"
                                       "n = -42\nbig = 1_000_000\nplus = +7\nzero = 0\n"
                                       "s = \"hello\"\nempty = \"\"\n"
                                       "\"quoted key\" = \"v\"\nbare-key_2 = 3\n"
                                       "tight=5#comment right after\n"
                                       "   indented   =   \"x\"   # trailing comment\n"),
                        &d));
        QCOMPARE(d.value(QString()).value(QStringLiteral("top")), QVariant(qlonglong(1)));
        const toml::Table t = d.value(QStringLiteral("t"));
        QCOMPARE(t.value(QStringLiteral("yes")).typeId(), int(QMetaType::Bool));
        QCOMPARE(t.value(QStringLiteral("yes")).toBool(), true);
        QCOMPARE(t.value(QStringLiteral("no")).toBool(), false);
        QCOMPARE(t.value(QStringLiteral("n")).typeId(), int(QMetaType::LongLong));
        QCOMPARE(t.value(QStringLiteral("n")).toLongLong(), -42);
        QCOMPARE(t.value(QStringLiteral("big")).toLongLong(), 1000000);
        QCOMPARE(t.value(QStringLiteral("plus")).toLongLong(), 7);
        QCOMPARE(t.value(QStringLiteral("zero")).toLongLong(), 0);
        QCOMPARE(t.value(QStringLiteral("s")).toString(), QStringLiteral("hello"));
        QCOMPARE(t.value(QStringLiteral("empty")).typeId(), int(QMetaType::QString));
        QCOMPARE(t.value(QStringLiteral("quoted key")).toString(), QStringLiteral("v"));
        QCOMPARE(t.value(QStringLiteral("bare-key_2")).toLongLong(), 3);
        QCOMPARE(t.value(QStringLiteral("tight")).toLongLong(), 5);
        QCOMPARE(t.value(QStringLiteral("indented")).toString(), QStringLiteral("x"));
        QCOMPARE(t.size(), 12);
    }

    void tomlStringsAndEscapes()
    {
        toml::Document d;
        QVERIFY(parseOk(QStringLiteral("a = \"q\\\"b\\\\n\\nt\\tr\\r\"\n"
                                       "u = \"\\u00e9\\u0041\\uAC00\"\n"
                                       "k = \"한글 # not a comment\"\n"),
                        &d));
        const toml::Table t = d.value(QString());
        QCOMPARE(t.value(QStringLiteral("a")).toString(), QStringLiteral("q\"b\\n\nt\tr\r"));
        QCOMPARE(t.value(QStringLiteral("u")).toString(), QStringLiteral("éA가"));
        QCOMPARE(t.value(QStringLiteral("k")).toString(), QStringLiteral("한글 # not a comment"));
    }

    void tomlArrays()
    {
        toml::Document d;
        QVERIFY(parseOk(QStringLiteral("empty = []\n"
                                       "spaced = [ ]\n"
                                       "one = [\"a\"]\n"
                                       "multi = [\n"
                                       "  \"x\", # comment inside\n"
                                       "\n"
                                       "  \"y\",\n"
                                       "]\n"
                                       "tables = [\n"
                                       "  { label = \"L\", terminal = false, command = \"c \\\"q\\\"\" },\n"
                                       "  { id = \"zip\", terminal = true },\n"
                                       "  {},\n"
                                       "]\n"
                                       "after = 1\n"),
                        &d));
        const toml::Table t = d.value(QString());
        QCOMPARE(t.value(QStringLiteral("empty")).typeId(), int(QMetaType::QStringList));
        QVERIFY(t.value(QStringLiteral("empty")).toStringList().isEmpty());
        QVERIFY(t.value(QStringLiteral("spaced")).toStringList().isEmpty());
        QCOMPARE(t.value(QStringLiteral("one")).toStringList(), QStringList{QStringLiteral("a")});
        QCOMPARE(t.value(QStringLiteral("multi")).toStringList(), (QStringList{QStringLiteral("x"), QStringLiteral("y")}));
        const QVariantList rows = t.value(QStringLiteral("tables")).toList();
        QCOMPARE(t.value(QStringLiteral("tables")).typeId(), int(QMetaType::QVariantList));
        QCOMPARE(rows.size(), 3);
        const QVariantMap r0 = rows[0].toMap();
        QCOMPARE(r0.value(QStringLiteral("label")).toString(), QStringLiteral("L"));
        QCOMPARE(r0.value(QStringLiteral("terminal")).typeId(), int(QMetaType::Bool));
        QCOMPARE(r0.value(QStringLiteral("terminal")).toBool(), false);
        QCOMPARE(r0.value(QStringLiteral("command")).toString(), QStringLiteral("c \"q\""));
        QCOMPARE(rows[1].toMap().value(QStringLiteral("terminal")).toBool(), true);
        QVERIFY(rows[2].toMap().isEmpty());
        QCOMPARE(t.value(QStringLiteral("after")).toLongLong(), 1);
    }

    void tomlTables()
    {
        toml::Document d;
        QVERIFY(parseOk(QStringLiteral("[a]\nx = 1\n[ \"b c\" ]\nx = 2\n[empty]\n[a]\ny = 3\n"), &d));
        QCOMPARE(d.value(QStringLiteral("a")).value(QStringLiteral("x")).toLongLong(), 1);
        QCOMPARE(d.value(QStringLiteral("a")).value(QStringLiteral("y")).toLongLong(), 3); // reopened table adds
        QCOMPARE(d.value(QStringLiteral("b c")).value(QStringLiteral("x")).toLongLong(), 2);
        QVERIFY(d.contains(QStringLiteral("empty")));
        QVERIFY(d.contains(QString())); // the root table always exists
        // The same key in two tables is fine.
        QVERIFY(parseOk(QStringLiteral("[a]\nk = 1\n[b]\nk = 1\n")));
    }

    void tomlLineEndings()
    {
        toml::Document d;
        QVERIFY(parseOk(QStringLiteral("[t]\r\na = 1\r\nb = [\r\n  \"x\",\r\n]\r\n# c\r\n\r\n"), &d));
        QCOMPARE(d.value(QStringLiteral("t")).value(QStringLiteral("a")).toLongLong(), 1);
        QCOMPARE(d.value(QStringLiteral("t")).value(QStringLiteral("b")).toStringList(), QStringList{QStringLiteral("x")});
        QVERIFY(parseOk(QString()));
        QVERIFY(parseOk(QStringLiteral("# only a comment")));
        QVERIFY(parseOk(QStringLiteral("a = 1"))); // no final newline
        QVERIFY(parseOk(QStringLiteral("\t a = 1 \t\n")));
    }

    void tomlErrors_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("line");
        QTest::addColumn<QString>("message"); // part of the (Korean) message
        QTest::newRow("unterminated string") << QStringLiteral("a = \"abc\n") << 1 << QStringLiteral("닫히지");
        QTest::newRow("unterminated at eof") << QStringLiteral("a = \"abc") << 1 << QStringLiteral("닫히지");
        QTest::newRow("backslash at eof") << QStringLiteral("a = \"abc\\") << 1 << QStringLiteral("닫히지");
        QTest::newRow("bad escape") << QStringLiteral("a = \"\\x41\"") << 1 << QStringLiteral("이스케이프 \\x");
        QTest::newRow("bad \\u") << QStringLiteral("a = \"\\uZZZZ\"") << 1 << QStringLiteral("\\u");
        QTest::newRow("short \\u") << QStringLiteral("a = \"\\u41\"") << 1 << QStringLiteral("\\u");
        QTest::newRow("no key") << QStringLiteral("= 1") << 1 << QStringLiteral("키가 없습니다");
        QTest::newRow("no equals") << QStringLiteral("\n\nkey 1") << 3 << QStringLiteral("'key' 뒤에 = 가");
        QTest::newRow("no value") << QStringLiteral("a =") << 1 << QStringLiteral("값이 없습니다");
        QTest::newRow("no value, comment") << QStringLiteral("a = # c") << 1 << QStringLiteral("값이 없습니다");
        QTest::newRow("bare word") << QStringLiteral("a = hello") << 1 << QStringLiteral("알 수 없는 값 'hello'");
        QTest::newRow("capital True") << QStringLiteral("a = True") << 1 << QStringLiteral("'True'");
        QTest::newRow("hex int") << QStringLiteral("a = 0x10") << 1 << QStringLiteral("'0x10'");
        QTest::newRow("float") << QStringLiteral("a = 1.5") << 1 << QStringLiteral("'1.5'");
        QTest::newRow("overflow") << QStringLiteral("a = 99999999999999999999") << 1 << QStringLiteral("알 수 없는 값");
        QTest::newRow("single quotes") << QStringLiteral("a = 'x'") << 1 << QStringLiteral("알 수 없는 값");
        QTest::newRow("trailing garbage") << QStringLiteral("a = 1 2") << 1 << QStringLiteral("값 뒤에");
        QTest::newRow("garbage after string") << QStringLiteral("a = \"x\" y") << 1 << QStringLiteral("값 뒤에");
        QTest::newRow("duplicate key") << QStringLiteral("[t]\na = 1\n\na = 2\n") << 4 << QStringLiteral("'a'이(가) 두 번");
        QTest::newRow("duplicate in reopened table") << QStringLiteral("[t]\na = 1\n[u]\n[t]\na = 2") << 5 << QStringLiteral("두 번");
        QTest::newRow("table without ]") << QStringLiteral("[t\n") << 1 << QStringLiteral("]가 필요");
        QTest::newRow("dotted table") << QStringLiteral("[a.b]") << 1 << QStringLiteral("]가 필요");
        QTest::newRow("empty table name") << QStringLiteral("x = 1\n[]") << 2 << QStringLiteral("키가 없습니다");
        QTest::newRow("garbage after table") << QStringLiteral("[t] x") << 1 << QStringLiteral("값 뒤에");
        QTest::newRow("array no comma") << QStringLiteral("a = [\"x\" \"y\"]") << 1 << QStringLiteral("쉼표");
        QTest::newRow("array of ints") << QStringLiteral("a = [1, 2]") << 1 << QStringLiteral("한 가지만");
        QTest::newRow("mixed array") << QStringLiteral("a = [\"x\", { k = \"v\" }]") << 1 << QStringLiteral("한 가지만");
        QTest::newRow("mixed array 2") << QStringLiteral("a = [{ k = \"v\" }, \"x\"]") << 1 << QStringLiteral("한 가지만");
        QTest::newRow("unclosed array") << QStringLiteral("a = [\n\"x\",\n\n") << 4 << QStringLiteral("한 가지만");
        QTest::newRow("error line in array") << QStringLiteral("[t]\na = [\n  \"x\",\n  oops,\n]") << 4 << QStringLiteral("'a'");
        QTest::newRow("inline no comma") << QStringLiteral("a = [{ k = \"v\" j = \"w\" }]") << 1 << QStringLiteral("{ } 안의");
        QTest::newRow("inline int value") << QStringLiteral("a = [{ k = 1 }]") << 1 << QStringLiteral("'k'의 값은");
        QTest::newRow("inline no equals") << QStringLiteral("a = [{ k \"v\" }]") << 1 << QStringLiteral("'k' 뒤에 = 가");
        QTest::newRow("inline no key") << QStringLiteral("a = [{ = \"v\" }]") << 1 << QStringLiteral("키가 없습니다");
        QTest::newRow("inline bad string") << QStringLiteral("a = [{ k = \"v }]") << 1 << QStringLiteral("닫히지");
        QTest::newRow("inline truex") << QStringLiteral("a = [{ k = truex }]") << 1 << QStringLiteral("{ } 안의");
        QTest::newRow("inline across lines") << QStringLiteral("a = [{ k = \"v\",\n j = \"w\" }]") << 1 << QStringLiteral("키가 없습니다");
        QTest::newRow("bare inline table") << QStringLiteral("a = { k = \"v\" }") << 1 << QStringLiteral("알 수 없는 값");
        QTest::newRow("bad string in array") << QStringLiteral("a = [\"\\q\"]") << 1 << QStringLiteral("이스케이프");
        QTest::newRow("bad quoted key") << QStringLiteral("\"k = 1") << 1 << QStringLiteral("닫히지");
        QTest::newRow("bad quoted table") << QStringLiteral("[\"t]") << 1 << QStringLiteral("닫히지");
    }
    void tomlErrors()
    {
        QFETCH(QString, text);
        QFETCH(int, line);
        QFETCH(QString, message);
        toml::Error e;
        QVERIFY(!parseOk(text, nullptr, &e));
        QCOMPARE(e.line, line);
        QVERIFY2(e.message.contains(message), qPrintable(e.message));
    }

    void tomlQuoteAndKey()
    {
        QCOMPARE(toml::quote(QString()), QStringLiteral("\"\""));
        QCOMPARE(toml::quote(QStringLiteral("a\"b\\c\nd\te\rf")), QStringLiteral("\"a\\\"b\\\\c\\nd\\te\\rf\""));
        QCOMPARE(toml::quote(QStringLiteral("\x01\x1f\x7f")), QStringLiteral("\"\\u0001\\u001f\\u007f\""));
        QCOMPARE(toml::quote(QStringLiteral("한글 #")), QStringLiteral("\"한글 #\""));
        QCOMPARE(toml::key(QStringLiteral("abc")), QStringLiteral("abc"));
        QCOMPARE(toml::key(QStringLiteral("a-b_C9")), QStringLiteral("a-b_C9"));
        QCOMPARE(toml::key(QString()), QStringLiteral("\"\""));
        QCOMPARE(toml::key(QStringLiteral("tar.gz")), QStringLiteral("\"tar.gz\""));
        QCOMPARE(toml::key(QStringLiteral("a b")), QStringLiteral("\"a b\""));
        QCOMPARE(toml::key(QStringLiteral("열기")), QStringLiteral("\"열기\"")); // non-ASCII is quoted
        QCOMPARE(toml::key(QStringLiteral("a=\"1\"")), QStringLiteral("\"a=\\\"1\\\"\""));
    }

    void tomlValue()
    {
        QCOMPARE(toml::value(true), QStringLiteral("true"));
        QCOMPARE(toml::value(false), QStringLiteral("false"));
        QCOMPARE(toml::value(42), QStringLiteral("42"));
        QCOMPARE(toml::value(qlonglong(-7)), QStringLiteral("-7"));
        QCOMPARE(toml::value(uint(5)), QStringLiteral("5"));
        QCOMPARE(toml::value(QStringLiteral("x\"y")), QStringLiteral("\"x\\\"y\""));
        QCOMPARE(toml::value(QStringList()), QStringLiteral("[]"));
        QCOMPARE(toml::value(QStringList{QStringLiteral("a"), QStringLiteral("b\"")}), QStringLiteral("[\"a\", \"b\\\"\"]"));
        QCOMPARE(toml::value(QVariantList()), QStringLiteral("[]"));
        // Inline tables: id, label, key, terminal, extensions first, the rest sorted, command last.
        const QVariantMap row{{QStringLiteral("command"), QStringLiteral("echo {files}")},
                              {QStringLiteral("zeta"), QStringLiteral("z")},
                              {QStringLiteral("alpha"), QStringLiteral("a")},
                              {QStringLiteral("terminal"), false},
                              {QStringLiteral("key"), QStringLiteral("K")},
                              {QStringLiteral("label"), QStringLiteral("L")},
                              {QStringLiteral("id"), QStringLiteral("zip")}};
        QCOMPARE(toml::value(QVariantList{row, QVariantMap{{QStringLiteral("color"), QStringLiteral("#112233")},
                                                           {QStringLiteral("extensions"), QStringLiteral("a, b")}}}),
                 QStringLiteral("[\n"
                                "  { id = \"zip\", label = \"L\", key = \"K\", terminal = false, alpha = \"a\", zeta = \"z\", command = \"echo {files}\" },\n"
                                "  { extensions = \"a, b\", color = \"#112233\" },\n"
                                "]"));
    }

    // Whatever value() writes, parse() reads back unchanged: every control character, quotes,
    // backslashes, non-BMP characters, in plain strings, string arrays and inline tables.
    void tomlRoundTrip()
    {
        QRandomGenerator rng(20261003);
        const QString pool = QStringLiteral("aZ09 _-#=[]{},.\"'\\$`\t\n\r\x01\x1b\x7f한글é") + QString::fromUtf8("\xf0\x9f\x98\x80");
        for (int round = 0; round < 300; ++round) {
            auto randomString = [&] {
                QString s;
                const int n = rng.bounded(12);
                for (int i = 0; i < n; ++i) {
                    if (rng.bounded(6) == 0)
                        s += QChar(char16_t(rng.bounded(0x20))); // any C0 control
                    else
                        s += pool[rng.bounded(pool.size())];
                }
                return s;
            };
            const QString s = randomString(), k = randomString();
            const QStringList list{randomString(), randomString()};
            const QVariantList tables{QVariantMap{{QStringLiteral("label"), randomString()},
                                                  {QStringLiteral("terminal"), bool(round & 1)},
                                                  {QStringLiteral("command"), randomString()}}};
            const QString text = QStringLiteral("[%1]\n%2 = %3\nlist = %4\ntables = %5\nn = %6\n")
                                     .arg(toml::key(k), toml::key(QStringLiteral("s")), toml::value(s), toml::value(list),
                                          toml::value(tables), toml::value(round - 150));
            toml::Document d;
            toml::Error e;
            QVERIFY2(toml::parse(text, d, e), qPrintable(QStringLiteral("line %1: %2\n%3").arg(e.line).arg(e.message, text)));
            const toml::Table t = d.value(k);
            QCOMPARE(t.value(QStringLiteral("s")).toString(), s);
            QCOMPARE(t.value(QStringLiteral("list")).toStringList(), list);
            QCOMPARE(t.value(QStringLiteral("tables")).toList(), tables);
            QCOMPARE(t.value(QStringLiteral("n")).toLongLong(), qlonglong(round - 150));
            // One value per line: a written string never spans lines.
            QCOMPARE(toml::value(s).count(QLatin1Char('\n')), 0);
        }
    }

    // ---------------------------------------------------------------- Settings: check()

    void settingsCheck_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QStringList>("expected"); // one entry per problem: a part of its message
        const QStringList none;
        QTest::newRow("empty file") << QString() << none;
        QTest::newRow("comments") << QStringLiteral("# just\n# comments\n") << none;
        QTest::newRow("syntax error") << QStringLiteral("[view]\nicon_size = = 3\n") << QStringList{QStringLiteral("2번째 줄")};

        QTest::newRow("bool ok") << QStringLiteral("[view]\nstripes = false\n") << none;
        QTest::newRow("bool as int") << QStringLiteral("[view]\nstripes = 1\n") << QStringList{QStringLiteral("view.stripes: true 또는 false")};
        QTest::newRow("bool as string") << QStringLiteral("[view]\nstripes = \"true\"\n") << QStringList{QStringLiteral("view.stripes")};

        QTest::newRow("int min") << QStringLiteral("[view]\nicon_size = 32\n") << none;
        QTest::newRow("int max") << QStringLiteral("[view]\nicon_size = 256\n") << none;
        QTest::newRow("int below") << QStringLiteral("[view]\nicon_size = 31\n") << QStringList{QStringLiteral("view.icon_size: 32..256")};
        QTest::newRow("int above") << QStringLiteral("[view]\nicon_size = 257\n") << QStringList{QStringLiteral("view.icon_size: 32..256")};
        QTest::newRow("int negative") << QStringLiteral("[view]\nfont_size = -1\n") << QStringList{QStringLiteral("view.font_size: 0..32")};
        QTest::newRow("int huge") << QStringLiteral("[preview]\nvolume = 9223372036854775807\n") << QStringList{QStringLiteral("preview.volume: 0..100")};
        QTest::newRow("int as string") << QStringLiteral("[view]\nicon_size = \"96\"\n") << QStringList{QStringLiteral("view.icon_size")};
        QTest::newRow("int as bool") << QStringLiteral("[terminal]\nfont_size = true\n") << QStringList{QStringLiteral("terminal.font_size: 9..24")};

        QTest::newRow("string ok") << QStringLiteral("[terminal]\nshell = \"/bin/zsh\"\n") << none;
        QTest::newRow("string empty") << QStringLiteral("[terminal]\nshell = \"\"\n") << none;
        QTest::newRow("string as int") << QStringLiteral("[terminal]\nshell = 5\n") << QStringList{QStringLiteral("terminal.shell: \"문자열\"")};

        QTest::newRow("color ok") << QStringLiteral("[file_colors]\nfolder = \"#a1B2c3\"\n") << none;
        QTest::newRow("color empty") << QStringLiteral("[file_colors]\nfolder = \"\"\n") << none;
        QTest::newRow("color short") << QStringLiteral("[file_colors]\nfolder = \"#abc\"\n") << QStringList{QStringLiteral("file_colors.folder: \"#RRGGBB\"")};
        QTest::newRow("color name") << QStringLiteral("[file_colors]\nfolder = \"red\"\n") << QStringList{QStringLiteral("file_colors.folder")};
        QTest::newRow("color no hash") << QStringLiteral("[file_colors]\nfolder = \"A1B2C3\"\n") << QStringList{QStringLiteral("file_colors.folder")};
        QTest::newRow("color not hex") << QStringLiteral("[file_colors]\nfolder = \"#GGGGGG\"\n") << QStringList{QStringLiteral("file_colors.folder")};
        QTest::newRow("color bool") << QStringLiteral("[file_colors]\nfolder = false\n") << QStringList{QStringLiteral("file_colors.folder")};

        QTest::newRow("choice ok") << QStringLiteral("[view]\ndefault_mode = \"columns\"\n[general]\nlanguage = \"zh_CN\"\n") << none;
        QTest::newRow("choice wrong") << QStringLiteral("[view]\ndefault_mode = \"grid\"\n")
                                      << QStringList{QStringLiteral("view.default_mode: 허용값은 [\"list\", \"gallery\", \"columns\"]")};
        QTest::newRow("choice case") << QStringLiteral("[appearance]\ntheme = \"Dark\"\n") << QStringList{QStringLiteral("appearance.theme")};
        QTest::newRow("choice zh") << QStringLiteral("[general]\nlanguage = \"zh\"\n") << QStringList{QStringLiteral("general.language")};

        QTest::newRow("list ok") << QStringLiteral("[sidebar]\nfavorites = [\"/a\", \"/b\"]\n") << none;
        QTest::newRow("list empty") << QStringLiteral("[sidebar]\nfavorites = []\n") << none;
        QTest::newRow("list as string") << QStringLiteral("[sidebar]\nfavorites = \"/a\"\n") << QStringList{QStringLiteral("sidebar.favorites: [\"문자열\", ...]")};
        QTest::newRow("list of tables") << QStringLiteral("[sidebar]\nfavorites = [{ a = \"b\" }]\n") << QStringList{QStringLiteral("sidebar.favorites")};

        const QString cmds = QStringLiteral("[selection_menu]\ncommands = %1\n");
        QTest::newRow("commands empty") << cmds.arg(QStringLiteral("[]")) << none;
        QTest::newRow("commands ok") << cmds.arg(QStringLiteral("[\n { label = \"L\", key = \"a\", terminal = false, command = \"c\" },\n"
                                                                " { id = \"zip\", label = \"Z\", command = \"z\" },\n { label = \"M\", key = \"9\", command = \"m\" },\n]"))
                                     << none;
        QTest::newRow("commands not array") << cmds.arg(QStringLiteral("\"x\"")) << QStringList{QStringLiteral("selection_menu.commands: [ { label")};
        QTest::newRow("commands strings") << cmds.arg(QStringLiteral("[\"x\"]")) << QStringList{QStringLiteral("selection_menu.commands: [ { label")};
        QTest::newRow("command without command") << cmds.arg(QStringLiteral("[{ label = \"L\" }]"))
                                                 << QStringList{QStringLiteral("1번째 항목에 label과 command")};
        QTest::newRow("command blank label") << cmds.arg(QStringLiteral("[{ label = \"x\", command = \"c\" }, { label = \"  \", command = \"c\" }]"))
                                             << QStringList{QStringLiteral("2번째 항목에 label과 command")};
        QTest::newRow("command two-letter key") << cmds.arg(QStringLiteral("[{ label = \"L\", key = \"AB\", command = \"c\" }]"))
                                                << QStringList{QStringLiteral("key는 글자 하나")};
        QTest::newRow("command korean key") << cmds.arg(QStringLiteral("[{ label = \"L\", key = \"가\", command = \"c\" }]"))
                                            << QStringList{QStringLiteral("key는 글자 하나")};
        QTest::newRow("command symbol key") << cmds.arg(QStringLiteral("[{ label = \"L\", key = \"-\", command = \"c\" }]"))
                                            << QStringList{QStringLiteral("key는 글자 하나")};
        QTest::newRow("command bool label") << cmds.arg(QStringLiteral("[{ label = true, command = \"c\" }]"))
                                            << QStringList{QStringLiteral("1번째 항목에 label과 command")};
        QTest::newRow("command bool command") << cmds.arg(QStringLiteral("[{ label = \"L\", command = true }]"))
                                              << QStringList{QStringLiteral("1번째 항목에 label과 command")};
        QTest::newRow("command bool key") << cmds.arg(QStringLiteral("[{ label = \"L\", key = true, command = \"c\" }]"))
                                          << QStringList{QStringLiteral("key는 글자 하나")};
        QTest::newRow("command terminal string") << cmds.arg(QStringLiteral("[{ label = \"L\", terminal = \"yes\", command = \"c\" }]"))
                                                 << QStringList{QStringLiteral("terminal은 true 또는 false")};
        QTest::newRow("command unknown id") << cmds.arg(QStringLiteral("[{ id = \"mine\", label = \"L\", command = \"c\" }]"))
                                            << QStringList{QStringLiteral("id는 기본 명령 하나씩에만 씁니다 (new_folder, zip, ai)")};
        QTest::newRow("command duplicate id") << cmds.arg(QStringLiteral("[{ id = \"ai\", label = \"L\", command = \"c\" }, { id = \"ai\", label = \"M\", command = \"d\" }]"))
                                              << QStringList{QStringLiteral("2번째 항목의 id")};

        const QString colors = QStringLiteral("[file_colors]\ngroups = %1\n");
        QTest::newRow("colors empty") << colors.arg(QStringLiteral("[]")) << none;
        QTest::newRow("colors ok") << colors.arg(QStringLiteral("[{ extensions = \"a, b\", color = \"#00ff00\" }]")) << none;
        QTest::newRow("colors not array") << colors.arg(QStringLiteral("true")) << QStringList{QStringLiteral("file_colors.groups: [ { extensions")};
        QTest::newRow("colors no extensions") << colors.arg(QStringLiteral("[{ color = \"#00ff00\" }]")) << QStringList{QStringLiteral("extensions(쉼표로")};
        QTest::newRow("colors blank extensions") << colors.arg(QStringLiteral("[{ extensions = \" \", color = \"#00ff00\" }]")) << QStringList{QStringLiteral("extensions(쉼표로")};
        QTest::newRow("colors bool extensions") << colors.arg(QStringLiteral("[{ extensions = true, color = \"#00ff00\" }]")) << QStringList{QStringLiteral("extensions(쉼표로")};
        QTest::newRow("colors bad color") << colors.arg(QStringLiteral("[{ extensions = \"a\", color = \"green\" }]")) << QStringList{QStringLiteral("1번째 항목의 color")};

        QTest::newRow("unknown item") << QStringLiteral("[view]\nzoom = 1\n") << QStringList{QStringLiteral("view.zoom: 알 수 없는 항목")};
        QTest::newRow("unknown table") << QStringLiteral("[nope]\nx = 1\n") << QStringList{QStringLiteral("nope.x: 알 수 없는 항목")};
        QTest::newRow("key before any table") << QStringLiteral("icon_size = 96\n") << QStringList{QStringLiteral("icon_size: 알 수 없는 항목")};
        QTest::newRow("item in wrong table") << QStringLiteral("[general]\nicon_size = 96\n") << QStringList{QStringLiteral("general.icon_size")};
        QTest::newRow("old [ai] ignored") << QStringLiteral("[ai]\ncommand = \"x\"\nanything = 5\n") << none;
        QTest::newRow("removed native_title_bar ignored") << QStringLiteral("[appearance]\nnative_title_bar = true\n") << none;
        QTest::newRow("mac-only item accepted") << QStringLiteral("[general]\nskip_full_disk_access_prompt = true\n") << none;

        QTest::newRow("open_with ok") << QStringLiteral("[open_with]\nmd = \"/Applications/X.app\"\n\"tar.gz\" = \"x\"\n") << none;
        QTest::newRow("open_with empty") << QStringLiteral("[open_with]\nmd = \"\"\n") << QStringList{QStringLiteral("open_with.md: 앱 경로")};
        QTest::newRow("open_with int") << QStringLiteral("[open_with]\nmd = 1\n") << QStringList{QStringLiteral("open_with.md")};

        QTest::newRow("shortcut ok") << QStringLiteral("[shortcuts]\n\"열기\" = [\"Ctrl+O\", \"F3\"]\n\"새로운 탭에서 열기\" = []\n") << none;
        QTest::newRow("shortcut renamed") << QStringLiteral("[shortcuts]\n\"훑어보기\" = [\"Space\"]\n\"컨텍스트 메뉴 새 탭에서 열기\" = [\"E\"]\n") << none;
        QTest::newRow("shortcut unknown") << QStringLiteral("[shortcuts]\n\"없는 메뉴\" = []\n") << QStringList{QStringLiteral("shortcuts.없는 메뉴: 이런 메뉴 항목은 없습니다")};
        QTest::newRow("shortcut string") << QStringLiteral("[shortcuts]\n\"열기\" = \"Ctrl+O\"\n") << QStringList{QStringLiteral("shortcuts.열기: [\"Ctrl+O\", ...]")};
        QTest::newRow("shortcut empty key") << QStringLiteral("[shortcuts]\n\"열기\" = [\"Ctrl+O\", \"\"]\n")
                                            << QStringList{QStringLiteral("shortcuts.열기: 알 수 없는 키 [\"\"]")};

        QTest::newRow("several problems") << QStringLiteral("[view]\nicon_size = 1\nstripes = 2\n[nope]\nx = 1\n")
                                          << QStringList{QStringLiteral("nope.x"), QStringLiteral("view.icon_size"), QStringLiteral("view.stripes")};
    }
    void settingsCheck()
    {
        QFETCH(QString, text);
        QFETCH(QStringList, expected);
        const QStringList problems = Settings::check(text);
        QVERIFY2(problems.size() == expected.size(), qPrintable(problems.join(QStringLiteral(" | "))));
        for (int i = 0; i < expected.size(); ++i)
            QVERIFY2(problems[i].contains(expected[i]), qPrintable(problems[i] + QStringLiteral("  <-  expected: ") + expected[i]));
    }

    // A key name Qt doesn't know is reported, not stored as an unknown key.
    void settingsCheckRejectsUnknownKeys()
    {
        const QStringList problems = Settings::check(QStringLiteral("[shortcuts]\n\"열기\" = [\"Ctrl+O\", \"Hyper+Nope\"]\n"));
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems[0].contains(QStringLiteral("shortcuts.열기: 알 수 없는 키 [\"Hyper+Nope\"]")));
    }

    // A color is valid as #RRGGBB only; a trailing newline must not slip through.
    void settingsColorStrict()
    {
        QVERIFY(util::isHexColor(QStringLiteral("#A1b2C3")));
        QVERIFY(!util::isHexColor(QStringLiteral("#A1B2C3\n")));
        QVERIFY(Settings::check(QStringLiteral("[file_colors]\nfolder = \"#A1B2C3\\n\"\n")).size() == 1);
    }

    // ---------------------------------------------------------------- Settings: rendering

    void renderDefaultsIsValid()
    {
        const QString text = Settings::renderDefaults();
        QVERIFY(text.endsWith(QLatin1Char('\n')));
        toml::Document d;
        toml::Error e;
        QVERIFY2(toml::parse(text, d, e), qPrintable(QStringLiteral("%1: %2").arg(e.line).arg(e.message)));
        QVERIFY2(Settings::check(text).isEmpty(), qPrintable(Settings::check(text).join(QLatin1Char('\n'))));
        // Every table is there; the values are the defaults.
        for (const char *t : {"general", "view", "appearance", "file_colors", "preview", "terminal", "sidebar", "selection_menu",
                              "open_with", "shortcuts"})
            QVERIFY2(d.contains(QLatin1String(t)), t);
        QCOMPARE(d.value(QStringLiteral("general")).value(QStringLiteral("language")).toString(), QStringLiteral("system"));
        QCOMPARE(d.value(QStringLiteral("general")).value(QStringLiteral("restore_session")).toBool(), true);
        QCOMPARE(d.value(QStringLiteral("view")).value(QStringLiteral("icon_size")).toLongLong(), 96);
        QCOMPARE(d.value(QStringLiteral("view")).value(QStringLiteral("default_mode")).toString(), QStringLiteral("list"));
        QCOMPARE(d.value(QStringLiteral("preview")).value(QStringLiteral("volume")).toLongLong(), 80);
        QCOMPARE(d.value(QStringLiteral("terminal")).value(QStringLiteral("shell")).toString(), QString());
        QCOMPARE(d.value(QStringLiteral("file_colors")).value(QStringLiteral("folder")).toString(), QString());
#ifdef Q_OS_MACOS
        QVERIFY(d.value(QStringLiteral("general")).contains(QStringLiteral("skip_full_disk_access_prompt")));
#else
        QVERIFY(!d.value(QStringLiteral("general")).contains(QStringLiteral("skip_full_disk_access_prompt")));
        QVERIFY(!text.contains(QStringLiteral("skip_full_disk_access_prompt")));
#endif
        // Favorites and colors are commented out (they follow the app's defaults); commands are written.
        QVERIFY(!d.value(QStringLiteral("sidebar")).contains(QStringLiteral("favorites")));
        QVERIFY(text.contains(QStringLiteral("\n# favorites = [")));
        QVERIFY(!d.value(QStringLiteral("file_colors")).contains(QStringLiteral("groups")));
        QVERIFY(text.contains(QStringLiteral("\n# groups = [\n#   { extensions = \"exe, com")));
        const QVariantList commands = d.value(QStringLiteral("selection_menu")).value(QStringLiteral("commands")).toList();
        QCOMPARE(commands.size(), 3);
        QStringList ids;
        for (const QVariant &c : commands)
            ids << c.toMap().value(QStringLiteral("id")).toString();
        QCOMPARE(ids, (QStringList{QStringLiteral("new_folder"), QStringLiteral("zip"), QStringLiteral("ai")}));
        // Shortcuts: every entry as a commented default, nothing set.
        QVERIFY(d.value(QStringLiteral("shortcuts")).isEmpty());
        QVERIFY(d.value(QStringLiteral("open_with")).isEmpty());
        for (const Shortcuts::Entry &e : Shortcuts::entries())
            QVERIFY2(text.contains(QStringLiteral("\n# %1 = %2").arg(toml::key(e.id), toml::value(Shortcuts::toStrings(e.defaults)))),
                     qPrintable(e.id));
        // The comments describe each item: type, range and default.
        QVERIFY(text.contains(QStringLiteral("# 타입: 정수 32..256 (px). 기본값: 96.")));
        QVERIFY(text.contains(QStringLiteral("# 허용값: \"list\" | \"gallery\" | \"columns\". 기본값: \"list\".")));
        QVERIFY(text.contains(QStringLiteral("# 타입: 불리언 (true / false). 기본값: true.")));
        QVERIFY(text.contains(QStringLiteral("# 타입: 색 \"#RRGGBB\" 또는 \"\". 기본값: \"\".")));
        // Uncommenting the commented defaults gives a file that is still valid and means the same.
        QString uncommented;
        for (const QString &line : text.split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("# favorites = ")) || line.startsWith(QStringLiteral("# groups = ")) ||
                line.startsWith(QStringLiteral("#   { extensions")) || line == QStringLiteral("# ]"))
                uncommented += line.mid(2) + QLatin1Char('\n');
            else
                uncommented += line + QLatin1Char('\n');
        }
        QVERIFY2(Settings::check(uncommented).isEmpty(), qPrintable(Settings::check(uncommented).join(QLatin1Char('\n'))));
        QVERIFY(parseOk(uncommented, &d));
        QCOMPARE(d.value(QStringLiteral("file_colors")).value(QStringLiteral("groups")).toList(), Settings::defaultFileColors());
    }

    void defaultFileColors()
    {
        const QVariantList rows = Settings::defaultFileColors();
        QVERIFY(rows.size() >= 10);
        QStringList seen;
        for (const QVariant &r : rows) {
            const QVariantMap m = r.toMap();
            QVERIFY(util::isHexColor(m.value(QStringLiteral("color")).toString()));
            const QStringList exts = util::extensionList(m.value(QStringLiteral("extensions")).toString());
            QVERIFY(!exts.isEmpty());
            for (const QString &e : exts) {
                QVERIFY2(!seen.contains(e), qPrintable(QStringLiteral("extension in two groups: ") + e));
                seen << e;
            }
        }
        QVERIFY(Settings::check(QStringLiteral("[file_colors]\ngroups = ") + toml::value(rows) + QLatin1Char('\n')).isEmpty());
    }

    // --print-config's engine: the file's values, defaults for the rest and for wrong values.
    void renderEffective()
    {
        const QString dir = scratch(QStringLiteral("effective"));
        ConfigDirOverride over(dir);
        QStringList problems;
        // No file: the defaults, no problems, no file created.
        QCOMPARE(Settings::renderEffective(problems), Settings::renderDefaults());
        QVERIFY(problems.isEmpty());
        QVERIFY(!QFileInfo::exists(Settings::configPath()));

        QVERIFY(writeText(Settings::configPath(),
                          QStringLiteral("[view]\nstripes = false\nicon_size = 9999\n[open_with]\nMD = \"/x/Editor.app\"\n"
                                         "[shortcuts]\n\"열기\" = [\"F3\"]\n\"훑어보기\" = [\"Ctrl+Y\"]\n[sidebar]\nfavorites = [\"/one\"]\n")));
        const QString text = Settings::renderEffective(problems);
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems[0].contains(QStringLiteral("view.icon_size")));
        QVERIFY(text.contains(QStringLiteral("\nstripes = false\n")));
        QVERIFY(text.contains(QStringLiteral("\nicon_size = 96\n"))); // the wrong value shows its default
        QVERIFY(text.contains(QStringLiteral("\nmd = \"/x/Editor.app\"\n"))); // extension lower-cased
        QVERIFY(text.contains(QStringLiteral("\nfavorites = [\"/one\"]\n")));
        QVERIFY(text.contains(QStringLiteral("\n\"열기\" = [\"F3\"]   # 기본값: ")));
        QVERIFY(text.contains(QStringLiteral("\n\"퀵 뷰어\" = [\"Ctrl+Y\"]   # 기본값: "))); // the renamed item under its new id
        QVERIFY(Settings::check(text).isEmpty());

        // A syntax error: defaults and the error.
        problems.clear();
        QVERIFY(writeText(Settings::configPath(), QStringLiteral("[view]\n\nstripes = nope\n")));
        QCOMPARE(Settings::renderEffective(problems), Settings::renderDefaults());
        QCOMPARE(problems.size(), 1);
        QVERIFY2(problems[0].startsWith(QStringLiteral("3번째 줄: ")), qPrintable(problems[0]));
    }

    void uiLanguage()
    {
        const QString dir = scratch(QStringLiteral("language"));
        ConfigDirOverride over(dir);
        const QStringList known{QStringLiteral("ko"), QStringLiteral("en"), QStringLiteral("ja"), QStringLiteral("zh_CN")};
        const QString system = Settings::uiLanguage(); // no file: the system's language
        QVERIFY2(known.contains(system), qPrintable(system));
        QVERIFY(!QFileInfo::exists(Settings::configPath())); // reading it never creates the file
        for (const QString &l : known) {
            QVERIFY(writeText(Settings::configPath(), QStringLiteral("[general]\nlanguage = \"%1\"\n").arg(l)));
            QCOMPARE(Settings::uiLanguage(), l);
        }
        // "system", unknown values, wrong types and broken files fall back to the system's language.
        for (const QString &text : {QStringLiteral("[general]\nlanguage = \"system\"\n"), QStringLiteral("[general]\nlanguage = \"fr\"\n"),
                                    QStringLiteral("[general]\nlanguage = \"zh\"\n"), QStringLiteral("[general]\nlanguage = 1\n"),
                                    QStringLiteral("[general]\nlanguage = \"ja\"\n[[broken"), QStringLiteral("language = \"ja\"\n"),
                                    QString()}) {
            QVERIFY(writeText(Settings::configPath(), text));
            QCOMPARE(Settings::uiLanguage(), system);
        }
    }

    void configPathFollowsEnvironment()
    {
        ConfigDirOverride over(QStringLiteral("/some/where/../dir/"));
        QCOMPARE(Settings::configPath(), QStringLiteral("/some/dir/config.toml"));
        // Without the variable: the platform's own place (only computed here, nothing is read or written).
        qunsetenv("GIFILES_CONFIG_DIR");
        const QString path = Settings::configPath();
        QVERIFY(QDir::isAbsolutePath(path));
#if defined(Q_OS_LINUX)
        QVERIFY2(path.endsWith(QStringLiteral("/gifiles/config.toml")), qPrintable(path));
#else
        QVERIFY2(path.endsWith(QStringLiteral("/Gifiles/config.toml")), qPrintable(path));
#endif
    }

    // ---------------------------------------------------------------- Settings: the instance

    void instanceCreatesExplainedFile()
    {
        QVERIFY(!QFileInfo::exists(Settings::configPath()));
        Settings *s = Settings::instance();
        QVERIFY(s);
        QCOMPARE(QDir::cleanPath(QFileInfo(Settings::configPath()).absolutePath()), QDir::cleanPath(m_configDir));
        QTRY_VERIFY(QFileInfo::exists(Settings::configPath()));
        QTRY_COMPARE(readText(Settings::configPath()), Settings::renderDefaults());
        QVERIFY(s->problems().isEmpty());
        // Defaults for known keys, nothing for unknown ones.
        QCOMPARE(s->value(Settings::IconSize).toInt(), 96);
        QCOMPARE(s->flag(Settings::Stripes), true);
        QCOMPARE(s->value(Settings::DefaultMode).toString(), QStringLiteral("list"));
        QVERIFY(!s->value(QStringLiteral("view/nothing")).isValid());
        QVERIFY(!s->contains(Settings::IconSize));
        QCOMPARE(s->value(Settings::FileColors).toList(), Settings::defaultFileColors());
        QCOMPARE(s->selectionCommand(QStringLiteral("zip")).value(QStringLiteral("key")).toString(), QStringLiteral("Z"));
        QVERIFY(s->selectionCommand(QStringLiteral("nope")).isEmpty());
    }

    void setValueSavesAndSignals()
    {
        Settings *s = Settings::instance();
        QSignalSpy changed(s, &Settings::changed);
        s->setValue(Settings::IconSize, 128);
        QCOMPARE(s->value(Settings::IconSize).toInt(), 128);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.at(0).at(0).toString(), QString::fromLatin1(Settings::IconSize));
        QVERIFY(readText(Settings::configPath()).contains(QStringLiteral("\nicon_size = 128\n")));
        s->setValue(Settings::IconSize, 128); // unchanged: no signal
        QCOMPARE(changed.size(), 1);
        s->setValue(Settings::Stripes, true); // set to its default: stored, but nothing changed
        QVERIFY(s->contains(Settings::Stripes));
        QCOMPARE(changed.size(), 1);
        s->remove(Settings::Stripes);
        QVERIFY(!s->contains(Settings::Stripes));
        QCOMPARE(changed.size(), 1);
        s->setValue(QStringLiteral("open_with/md"), QStringLiteral("/Apps/Edit.app"));
        s->setValue(QStringLiteral("open_with/tar.gz"), QStringLiteral("/Apps/Unpack.app"));
        QStringList apps = s->keys(QStringLiteral("open_with"));
        apps.sort();
        QCOMPARE(apps, (QStringList{QStringLiteral("md"), QStringLiteral("tar.gz")}));
        const QString text = readText(Settings::configPath());
        QVERIFY(text.contains(QStringLiteral("\nmd = \"/Apps/Edit.app\"\n")));
        QVERIFY(text.contains(QStringLiteral("\n\"tar.gz\" = \"/Apps/Unpack.app\"\n")));
        QVERIFY(Settings::check(text).isEmpty());
        s->remove(QStringLiteral("open_with/md"));
        s->remove(QStringLiteral("open_with/tar.gz"));
        QVERIFY(s->keys(QStringLiteral("open_with")).isEmpty());
        s->remove(Settings::IconSize);
        QCOMPARE(s->value(Settings::IconSize).toInt(), 96);
        QTRY_COMPARE(readText(Settings::configPath()), Settings::renderDefaults());
    }

    // What the app writes, it reads back as the same values (values with every kind of quoting trouble).
    void renderRoundTrip()
    {
        Settings *s = Settings::instance();
        const QString nasty = QStringLiteral("a \"q\" \\ $x `y` \n\t #h 한글");
        const QVariantList commands{
            QVariantMap{{QStringLiteral("label"), nasty}, {QStringLiteral("key"), QStringLiteral("Q")}, {QStringLiteral("terminal"), false},
                        {QStringLiteral("command"), QStringLiteral("echo \"{files}\" '{names}' # \\n")}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("ai")}, {QStringLiteral("label"), QStringLiteral("AI")},
                        {QStringLiteral("key"), QStringLiteral("A")}, {QStringLiteral("terminal"), true}, {QStringLiteral("command"), QStringLiteral("codex {prompt}")}},
        };
        s->setValue(Settings::TermShell, nasty);
        s->setValue(Settings::Favorites, QStringList{QStringLiteral("/a \"b\""), QStringLiteral("C:\\Users\\x")});
        s->setValue(Settings::SelectionCommands, commands);
        s->setValue(Settings::FolderColor, QStringLiteral("#ABCDEF"));
        s->setValue(QStringLiteral("shortcuts/열기"), QStringList{QStringLiteral("Ctrl+Shift+O")});
        // On Windows a save can find the file still held (the watcher, a scanner) and land on a retry.
        QTRY_VERIFY(readText(Settings::configPath()).contains(QStringLiteral("Ctrl+Shift+O")));
        const QString text = readText(Settings::configPath());
        QVERIFY2(Settings::check(text).isEmpty(), qPrintable(Settings::check(text).join(QLatin1Char('\n'))));
        toml::Document d;
        QVERIFY(parseOk(text, &d));
        QCOMPARE(d.value(QStringLiteral("terminal")).value(QStringLiteral("shell")).toString(), nasty);
        QCOMPARE(d.value(QStringLiteral("sidebar")).value(QStringLiteral("favorites")).toStringList(),
                 (QStringList{QStringLiteral("/a \"b\""), QStringLiteral("C:\\Users\\x")}));
        QCOMPARE(d.value(QStringLiteral("selection_menu")).value(QStringLiteral("commands")).toList(), commands);
        QCOMPARE(d.value(QStringLiteral("shortcuts")).value(QStringLiteral("열기")).toStringList(), QStringList{QStringLiteral("Ctrl+Shift+O")});
        // Built-in commands left out of the list come back (after the user's own).
        const QVariantList effective = s->value(Settings::SelectionCommands).toList();
        QStringList ids;
        for (const QVariant &c : effective)
            ids << c.toMap().value(QStringLiteral("id")).toString();
        QCOMPARE(ids, (QStringList{QString(), QStringLiteral("ai"), QStringLiteral("new_folder"), QStringLiteral("zip")}));
        QCOMPARE(s->selectionCommand(QStringLiteral("ai")).value(QStringLiteral("command")).toString(), QStringLiteral("codex {prompt}"));
        for (const char *k : {Settings::TermShell, Settings::Favorites, Settings::SelectionCommands, Settings::FolderColor})
            s->remove(QLatin1String(k));
        s->remove(QStringLiteral("shortcuts/열기"));
        QTRY_COMPARE(readText(Settings::configPath()), Settings::renderDefaults()); // a save may land on a retry (Windows)
    }

    // An edit made outside the app applies at once; a wrong value is reported and the old one kept.
    void reloadKeepsPreviousValueOnError()
    {
        Settings *s = Settings::instance();
        s->setValue(Settings::IconSize, 128);
        QSignalSpy problems(s, &Settings::problemsFound);
        QSignalSpy changed(s, &Settings::changed);

        editConfig(QStringLiteral("[view]\nicon_size = 9999\nstripes = false\n[file_colors]\nfolder = \"#a1b2c3\"\n"));
        QTRY_COMPARE_WITH_TIMEOUT(s->flag(Settings::Stripes), false, 5000);
        QCOMPARE(s->value(Settings::IconSize).toInt(), 128); // the wrong value keeps the previous one
        QCOMPARE(s->value(Settings::FolderColor).toString(), QStringLiteral("#A1B2C3")); // colors upper-cased
        QTRY_COMPARE(problems.size(), 1);
        QCOMPARE(s->problems().size(), 1);
        QVERIFY(s->problems().at(0).contains(QStringLiteral("view.icon_size")));
        QVERIFY(changed.contains(QVariantList{QString::fromLatin1(Settings::Stripes)}));
        QVERIFY(changed.contains(QVariantList{QString::fromLatin1(Settings::FolderColor)}));
        QVERIFY(!changed.contains(QVariantList{QString::fromLatin1(Settings::IconSize)}));

        // Fixed: the new value applies and the problems are gone.
        changed.clear();
        editConfig(QStringLiteral("[view]\nicon_size = 64\n"));
        QTRY_COMPARE_WITH_TIMEOUT(s->value(Settings::IconSize).toInt(), 64, 5000);
        QCOMPARE(s->flag(Settings::Stripes), true); // the line is gone: back to the default
        QCOMPARE(s->value(Settings::FolderColor).toString(), QString());
        QVERIFY(s->problems().isEmpty());
        QVERIFY(changed.contains(QVariantList{QString::fromLatin1(Settings::IconSize)}));
        QVERIFY(changed.contains(QVariantList{QString::fromLatin1(Settings::Stripes)}));

        // A syntax error keeps every current value.
        problems.clear();
        editConfig(QStringLiteral("[view]\nicon_size = 32\nstripes = tru\n"));
        QTRY_COMPARE_WITH_TIMEOUT(problems.size(), 1, 5000);
        QVERIFY2(s->problems().at(0).startsWith(QStringLiteral("3번째 줄: ")), qPrintable(s->problems().at(0)));
        QCOMPARE(s->value(Settings::IconSize).toInt(), 64);

        // Removing the line: back to the default.
        editConfig(QStringLiteral("[view]\n"));
        QTRY_COMPARE_WITH_TIMEOUT(s->value(Settings::IconSize).toInt(), 96, 5000);
        QVERIFY(s->problems().isEmpty());
        QVERIFY(!s->contains(Settings::IconSize));

        // The app's next save writes the whole explained file again.
        s->setValue(Settings::IconSize, 100);
        s->remove(Settings::IconSize);
        QTRY_COMPARE(readText(Settings::configPath()), Settings::renderDefaults()); // a save may land on a retry (Windows)
    }

    // A save that failed is tried again later; an edit made outside meanwhile wins over that retry.
    // (Windows CI: the replace fails while something still holds the file, and the retry used to write
    // the app's values over the test's edit. Here a read-only folder makes the replace fail.)
    void failedSaveRetryKeepsOutsideEdit()
    {
#ifdef Q_OS_WIN
        QSKIP("a read-only folder doesn't stop creating files on Windows");
#else
        Settings *s = Settings::instance();
        const QString dir = QFileInfo(Settings::configPath()).absolutePath();
        const QFile::Permissions perms = QFile(dir).permissions();
        QVERIFY(QFile(dir).setPermissions(QFile::ReadOwner | QFile::ExeOwner));
        s->setValue(Settings::IconSize, 128); // QSaveFile can't create its temporary file: retried
        editConfig(QStringLiteral("[view]\nstripes = false\n"));
        QVERIFY(QFile(dir).setPermissions(perms));
        QTRY_COMPARE_WITH_TIMEOUT(s->flag(Settings::Stripes), false, 5000);
        QTest::qWait(1000); // every retry has had its turn
        QCOMPARE(readText(Settings::configPath()), QStringLiteral("[view]\nstripes = false\n"));
        QCOMPARE(s->flag(Settings::Stripes), false);
        QCOMPARE(s->value(Settings::IconSize).toInt(), 96); // the file has no icon_size: the later edit wins
        s->remove(Settings::Stripes);
        s->setValue(Settings::IconSize, 100);
        s->remove(Settings::IconSize);
        QTRY_COMPARE(readText(Settings::configPath()), Settings::renderDefaults()); // a save may land on a retry (Windows)
#endif
    }

    // The same rule for [shortcuts] and [open_with]: a wrong value there keeps the user's previous keys/app.
    void reloadKeepsPreviousShortcutAndApp()
    {
        Settings *s = Settings::instance();
        const QString key = QStringLiteral("shortcuts/열기"), app = QStringLiteral("open_with/md");
        s->setValue(key, QStringList{QStringLiteral("F3")});
        s->setValue(app, QStringLiteral("/Apps/Edit.app"));
        QSignalSpy problems(s, &Settings::problemsFound);
        editConfig(QStringLiteral("[open_with]\nmd = 5\n[shortcuts]\n\"열기\" = \"F4\"\n[view]\nicon_size = 33\n"));
        QTRY_COMPARE_WITH_TIMEOUT(s->value(Settings::IconSize).toInt(), 33, 5000);
        QCOMPARE(problems.size(), 1);
        QCOMPARE(s->problems().size(), 2);
        const QStringList keys = s->value(key).toStringList();
        const QString kept = s->value(app).toString();
        s->remove(key);
        s->remove(app);
        s->remove(Settings::IconSize);
        QTRY_COMPARE(readText(Settings::configPath()), Settings::renderDefaults()); // a save may land on a retry (Windows)
        QCOMPARE(keys, QStringList{QStringLiteral("F3")});
        QCOMPARE(kept, QStringLiteral("/Apps/Edit.app"));
    }

    void selectionCommandsEmptyListKeepsBuiltins()
    {
        Settings *s = Settings::instance();
        editConfig(QStringLiteral("[selection_menu]\ncommands = []\n[file_colors]\ngroups = []\n"));
        QTRY_VERIFY_WITH_TIMEOUT(s->contains(Settings::FileColors), 5000);
        QVERIFY(s->value(Settings::FileColors).toList().isEmpty()); // [] = no colors
        QCOMPARE(s->value(Settings::SelectionCommands).toList().size(), 3); // built-ins can't be removed
        QVERIFY(!Theme::fileBaseColor(QStringLiteral("a.zip")).isValid());
        editConfig(QStringLiteral("\n"));
        QTRY_VERIFY_WITH_TIMEOUT(!s->contains(Settings::FileColors), 5000);
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("a.zip")), QColor(QStringLiteral("#FA85EE")));
        s->setValue(Settings::IconSize, 100);
        s->remove(Settings::IconSize);
    }

    // ---------------------------------------------------------------- Theme

    void fileColors()
    {
        // Defaults (Mdir III hues), by extension, case-insensitive, last suffix only.
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("a.zip")), QColor(QStringLiteral("#FA85EE")));
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("A.ZIP")), QColor(QStringLiteral("#FA85EE")));
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("x.tar.gz")), QColor(QStringLiteral("#FA85EE")));
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("run.exe")), QColor(QStringLiteral("#8ACF39")));
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("notes.md")), QColor(QStringLiteral("#35D1C5")));
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("backup.$$$")), QColor(QStringLiteral("#FA998F")));
        QVERIFY(!Theme::fileBaseColor(QStringLiteral("README")).isValid());
        QVERIFY(!Theme::fileBaseColor(QStringLiteral(".zip")).isValid()); // a dotfile has no extension
        QVERIFY(!Theme::fileBaseColor(QStringLiteral("a.")).isValid());
        QVERIFY(!Theme::fileBaseColor(QStringLiteral("a.unknownext")).isValid());
        QVERIFY(!Theme::fileBaseColor(QString()).isValid());
        // Not installed, the theme is light: fileColor is the light-mode version.
        QVERIFY(!Theme::colors().dark);
        QCOMPARE(Theme::fileColor(QStringLiteral("a.zip")), Theme::lightModeColor(QColor(QStringLiteral("#FA85EE"))));
        QVERIFY(!Theme::fileColor(QStringLiteral("README")).isValid());

        // The user's groups: the first group naming an extension wins; extension lists are lenient.
        Settings *s = Settings::instance();
        s->setValue(Settings::FileColors, QVariantList{
                                              QVariantMap{{QStringLiteral("extensions"), QStringLiteral(".ZIP, foo")}, {QStringLiteral("color"), QStringLiteral("#111111")}},
                                              QVariantMap{{QStringLiteral("extensions"), QStringLiteral("zip bar")}, {QStringLiteral("color"), QStringLiteral("#222222")}},
                                          });
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("a.zip")), QColor(QStringLiteral("#111111")));
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("a.FOO")), QColor(QStringLiteral("#111111")));
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("a.bar")), QColor(QStringLiteral("#222222")));
        QVERIFY(!Theme::fileBaseColor(QStringLiteral("a.exe")).isValid());
        // The color picker's live preview overrides, then lets go.
        Theme::previewFileColor({QStringLiteral("bar")}, QColor(QStringLiteral("#333333")));
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("a.bar")), QColor(QStringLiteral("#333333")));
        Theme::previewFileColor({QStringLiteral("bar")}, QColor());
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("a.bar")), QColor(QStringLiteral("#222222")));
        s->remove(Settings::FileColors);
        QCOMPARE(Theme::fileBaseColor(QStringLiteral("a.zip")), QColor(QStringLiteral("#FA85EE")));

        // Folders: none by default; a set color, light mode version in the light theme.
        QVERIFY(!Theme::folderColor().isValid());
        s->setValue(Settings::FolderColor, QStringLiteral("#CD6A51"));
        QCOMPARE(Theme::folderBaseColor(), QColor(QStringLiteral("#CD6A51")));
        QCOMPARE(Theme::folderColor(), Theme::lightModeColor(QColor(QStringLiteral("#CD6A51"))));
        s->remove(Settings::FolderColor);
        QVERIFY(!Theme::folderColor().isValid());
    }

    // Light mode: every color gets the same perceived lightness (OKLab L 0.52), keeps its hue, stays in sRGB.
    void lightModeColor()
    {
        QList<QColor> inputs;
        for (const QVariant &r : Settings::defaultFileColors())
            inputs << QColor(r.toMap().value(QStringLiteral("color")).toString());
        inputs << QColor(Qt::red) << QColor(Qt::green) << QColor(Qt::blue) << QColor(Qt::yellow) << QColor(Qt::cyan)
               << QColor(QStringLiteral("#CD6A51"));
        for (const QColor &c : inputs) {
            const QColor out = Theme::lightModeColor(c);
            QVERIFY(out.isValid());
            const Lab in = oklab(c), o = oklab(out);
            QVERIFY2(std::abs(o.l - 0.52) < 0.01, qPrintable(c.name() + QStringLiteral(" -> ") + out.name()));
            const double hueIn = std::atan2(in.b, in.a), hueOut = std::atan2(o.b, o.a);
            double dh = std::abs(hueIn - hueOut);
            dh = std::min(dh, 2 * 3.14159265358979323846 - dh);
            QVERIFY2(dh < 0.08, qPrintable(c.name() + QStringLiteral(" -> ") + out.name()));
            // A little more chroma than given, or as much as fits.
            QVERIFY(std::hypot(o.a, o.b) <= std::hypot(in.a, in.b) * 1.1 + 0.01);
        }
        // Grays stay gray; black and white land on the same mid gray.
        for (const QColor &g : {QColor(Qt::white), QColor(Qt::black), QColor(QStringLiteral("#808080"))}) {
            const QColor out = Theme::lightModeColor(g);
            QVERIFY(std::abs(out.red() - out.green()) <= 1 && std::abs(out.green() - out.blue()) <= 1);
            QVERIFY(std::abs(oklab(out).l - 0.52) < 0.01);
        }
        QCOMPARE(Theme::lightModeColor(Qt::white), Theme::lightModeColor(Qt::black));
    }

    // The selection bar: each item color at 80% of its perceived lightness, same hue, in sRGB.
    void selectionFillIsDarker()
    {
        QList<QColor> inputs{Theme::plainSelectionColor()};
        for (const QVariant &r : Settings::defaultFileColors())
            inputs << QColor(r.toMap().value(QStringLiteral("color")).toString());
        inputs << QColor(Qt::yellow) << QColor(Qt::blue) << QColor(QStringLiteral("#CD6A51"));
        for (const QColor &c : inputs) {
            const QColor out = Theme::selectionFill(c);
            const Lab in = oklab(c), o = oklab(out);
            QVERIFY2(std::abs(o.l - in.l * 0.8) < 0.01, qPrintable(c.name() + QStringLiteral(" -> ") + out.name()));
            if (std::hypot(in.a, in.b) > 0.02) { // grays have no hue to keep
                double dh = std::abs(std::atan2(in.b, in.a) - std::atan2(o.b, o.a));
                dh = std::min(dh, 2 * 3.14159265358979323846 - dh);
                QVERIFY2(dh < 0.08, qPrintable(c.name() + QStringLiteral(" -> ") + out.name()));
            }
        }
    }

    // ---------------------------------------------------------------- Recent folders

    // Only folders where something was done count, newest first; looking around doesn't.
    void recentFoldersFromRecords()
    {
        RecentFolders *r = RecentFolders::instance();
        r->clear();
        QSignalSpy changed(r, &RecentFolders::changed);
        const QString base = scratch(QStringLiteral("recent"));
        auto d = [&](const QString &rel) {
            QDir().mkpath(QDir(base).filePath(rel));
            return QDir(base).filePath(rel);
        };
        const QString a = d(QStringLiteral("a")), b = d(QStringLiteral("b")), c = d(QStringLiteral("c")), e = d(QStringLiteral("e"));
        // A move: from a into b. The destination is the newest.
        r->noteRecord({QStringLiteral("이동"), {{Step::Move, a + QStringLiteral("/x"), b + QStringLiteral("/x")}}});
        QCOMPARE(r->folders(), (QStringList{b, a}));
        QVERIFY(!changed.isEmpty());
        // A copy counts where it lands only (the source was just read).
        r->noteRecord({QStringLiteral("복사"), {{Step::Copy, e + QStringLiteral("/y"), c + QStringLiteral("/y")}}});
        QCOMPARE(r->folders(), (QStringList{c, b, a}));
        // Trash: where the item was; a new folder, an extracted archive: where they were made.
        r->noteRecord({QStringLiteral("휴지통으로 이동"), {{Step::Trash, a + QStringLiteral("/z"), QStringLiteral("/trash/z")}}});
        QCOMPARE(r->folders().first(), a);
        r->noteRecord({QStringLiteral("새로운 폴더"), {{Step::Mkdir, QString(), e + QStringLiteral("/new")}}});
        QCOMPARE(r->folders().first(), e);
        r->noteRecord({QStringLiteral("압축 풀기"), {{Step::Extract, b + QStringLiteral("/p.zip"), b + QStringLiteral("/p")}}});
        QCOMPARE(r->folders(), (QStringList{b, e, a, c})); // moved to the top, never twice
        // An empty record changes nothing.
        changed.clear();
        r->noteRecord({});
        QVERIFY(changed.isEmpty());
        // Paths are cleaned; files and missing folders aren't noted.
        r->note(c + QStringLiteral("/./"));
        QCOMPARE(r->folders().first(), c);
        QVERIFY(writeText(QDir(base).filePath(QStringLiteral("file.txt")), QStringLiteral("x")));
        r->note(QDir(base).filePath(QStringLiteral("file.txt")));
        r->note(QDir(base).filePath(QStringLiteral("missing")));
        QCOMPARE(r->folders().size(), 4);
        // A folder deleted since is left out of what is shown.
        QVERIFY(QDir(a).removeRecursively());
        QCOMPARE(r->folders(), (QStringList{c, b, e}));
        r->remove(b);
        QCOMPARE(r->folders(), (QStringList{c, e}));
        r->clear();
        QVERIFY(r->folders().isEmpty());
    }

    void recentFoldersCountSetting()
    {
        RecentFolders *r = RecentFolders::instance();
        r->clear();
        const QString base = scratch(QStringLiteral("recent-count"));
        QStringList made;
        for (int i = 0; i < 12; ++i) {
            made.prepend(QDir(base).filePath(QStringLiteral("f%1").arg(i)));
            QDir().mkpath(made.first());
            r->note(made.first());
        }
        QCOMPARE(Settings::instance()->value(Settings::RecentFoldersCount).toInt(), 8); // the default
        QCOMPARE(r->folders(), made.mid(0, 8));
        QSignalSpy changed(r, &RecentFolders::changed);
        Settings::instance()->setValue(Settings::RecentFoldersCount, 3);
        QVERIFY(!changed.isEmpty()); // the sidebar rebuilds
        QCOMPARE(r->folders(), made.mid(0, 3));
        Settings::instance()->setValue(Settings::RecentFoldersCount, 0); // none: the section goes away
        QVERIFY(r->folders().isEmpty());
        QVERIFY(!Settings::check(QStringLiteral("[sidebar]\nrecent_folders = -1\n")).isEmpty());
        QVERIFY(!Settings::check(QStringLiteral("[sidebar]\nrecent_folders = 51\n")).isEmpty());
        QVERIFY(Settings::check(QStringLiteral("[sidebar]\nrecent_folders = 0\n")).isEmpty());
        Settings::instance()->remove(Settings::RecentFoldersCount);
        // At most kKept are kept.
        for (int i = 0; i < RecentFolders::kKept + 5; ++i) {
            const QString f = QDir(base).filePath(QStringLiteral("g%1").arg(i));
            QDir().mkpath(f);
            r->note(f);
        }
        QCOMPARE(QSettings().value(QStringLiteral("recent/folders")).toStringList().size(), RecentFolders::kKept);
        r->clear();
    }

    void recentFoldersTerminalCommands()
    {
        for (const char *nav : {"cd ..", "cd", "  ls -la", "ls|less", "pwd", "clear", "pushd /tmp", "popd", "Set-Location C:\\", "gci", "exit", ""})
            QVERIFY2(RecentFolders::isNavigationCommand(QString::fromUtf8(nav)), nav);
        for (const char *work : {"touch a", "git commit -m x", "make", "rm -rf build", "lsof -i", "cdk deploy", "vim notes.txt", "npm test"})
            QVERIFY2(!RecentFolders::isNavigationCommand(QString::fromUtf8(work)), work);
        RecentFolders *r = RecentFolders::instance();
        r->clear();
        const QString dir = scratch(QStringLiteral("recent-term"));
        r->noteCommand(dir, QStringLiteral("ls"));
        QVERIFY(r->folders().isEmpty());
        r->noteCommand(dir, QStringLiteral("make"));
        QCOMPARE(r->folders(), QStringList{dir});
        r->clear();
        r->noteCommand(dir, QString()); // a line from the shell's history: unknown, counts
        QCOMPARE(r->folders(), QStringList{dir});
        r->clear();
    }

    // ---------------------------------------------------------------- Shortcuts

    void shortcutDefaults()
    {
        const auto *sc = Shortcuts::instance();
        // Finder's keys on every platform: Return renames, Command/Ctrl+Down opens.
        QCOMPARE(sc->defaults(QStringLiteral("열기")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Down"))});
        QCOMPARE(sc->defaults(QStringLiteral("이름 변경")), QList<QKeySequence>{QKeySequence(QStringLiteral("Return"))});
        QCOMPARE(sc->defaults(QStringLiteral("상위 폴더")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Up"))});
    }


    void shortcutIds()
    {
        QCOMPARE(Shortcuts::currentId(QStringLiteral("훑어보기")), QStringLiteral("퀵 뷰어"));
        QCOMPARE(Shortcuts::currentId(QStringLiteral("훑어보기 닫기")), QStringLiteral("퀵 뷰어 닫기"));
        QCOMPARE(Shortcuts::currentId(QStringLiteral("컨텍스트 메뉴 훑어보기")), QStringLiteral("컨텍스트 메뉴 퀵 뷰어"));
        QCOMPARE(Shortcuts::currentId(QStringLiteral("컨텍스트 메뉴 새 탭에서 열기")), QStringLiteral("컨텍스트 메뉴 새로운 탭에서 열기"));
        QCOMPARE(Shortcuts::currentId(QStringLiteral("열기")), QStringLiteral("열기"));
        QCOMPARE(Shortcuts::currentId(QString()), QString());
        QSet<QString> ids;
        for (const Shortcuts::Entry &e : Shortcuts::entries()) {
            QVERIFY2(!ids.contains(e.id), qPrintable(QStringLiteral("duplicate id ") + e.id));
            ids.insert(e.id);
            QVERIFY(!e.group.isEmpty());
            QCOMPARE(Shortcuts::currentId(e.id), e.id); // current ids aren't renamed again
            QCOMPARE(e.title(), e.id);                   // untranslated in tests
            // Every default key survives config.toml's text form.
            const QStringList strings = Shortcuts::toStrings(e.defaults);
            QCOMPARE(strings.size(), e.defaults.size());
            for (int i = 0; i < strings.size(); ++i) {
                QVERIFY2(!strings[i].isEmpty(), qPrintable(e.id));
                QCOMPARE(QKeySequence(strings[i], QKeySequence::PortableText), e.defaults[i]);
            }
        }
        for (const char *renamed : {"훑어보기", "컨텍스트 메뉴 훑어보기", "컨텍스트 메뉴 새 탭에서 열기"})
            QVERIFY(ids.contains(Shortcuts::currentId(QString::fromUtf8(renamed))));
        QCOMPARE(Shortcuts::toStrings({}), QStringList());
        QCOMPARE(Shortcuts::toStrings({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), QKeySequence(Qt::Key_F2),
                                       QKeySequence(Qt::META | Qt::Key_QuoteLeft), QKeySequence(Qt::ALT | Qt::Key_Up)}),
                 (QStringList{QStringLiteral("Ctrl+Shift+N"), QStringLiteral("F2"), QStringLiteral("Meta+`"), QStringLiteral("Alt+Up")}));
    }

    // ---------------------------------------------------------------- Util

    void revealCommand()
    {
        // Folders open as themselves; files open their folder with the file selected.
        const QString base = scratch(QStringLiteral("reveal"));
        auto p = [&](const QString &rel) { return QDir(base).filePath(rel); };
        auto np = [&](const QString &rel) { return QDir::toNativeSeparators(p(rel)); };
        Q_UNUSED(np);
        QVERIFY(QDir().mkpath(p(QStringLiteral("Alpha"))));
        QVERIFY(writeText(p(QStringLiteral("README.md")), QStringLiteral("x")));
        const util::Command dir = util::revealCommand(p("Alpha"));
        const util::Command file = util::revealCommand(p("README.md"));
#if defined(Q_OS_MACOS)
        QCOMPARE(dir.args, QStringList{p("Alpha")});
        QCOMPARE(file.args, (QStringList{QStringLiteral("-R"), p("README.md")}));
#elif defined(Q_OS_WIN)
        QCOMPARE(dir.args, QStringList{np("Alpha")});
        QCOMPARE(file.args, QStringList{QStringLiteral("/select,") + np("README.md")});
#else
        QVERIFY(dir.args.contains(QStringLiteral("org.freedesktop.FileManager1.ShowFolders")));
        QVERIFY(file.args.contains(QStringLiteral("org.freedesktop.FileManager1.ShowItems")));
        QVERIFY(file.args.contains(QStringLiteral("array:string:") + QUrl::fromLocalFile(p("README.md")).toString(QUrl::FullyEncoded)));
#endif
    }

    void translationsLoad()
    {
        // Korean is the source text; the other languages come from the committed .qm files, which
        // must cover every message (scripts/i18n.sh). Checked without installing them app-wide.
        const QString dir = QStringLiteral(GIFILES_SOURCE_DIR "/i18n");
        const QList<QPair<QString, QString>> open = {{QStringLiteral("en"), QStringLiteral("Open")},
                                                     {QStringLiteral("ja"), QString()},
                                                     {QStringLiteral("zh_CN"), QString()}};
        for (const auto &[lang, expected] : open) {
            QTranslator tr;
            QVERIFY2(tr.load(QStringLiteral("gifiles_%1").arg(lang), dir), qPrintable(lang));
            const QString t = tr.translate("Gifiles", "열기");
            QVERIFY2(!t.isEmpty() && t != QStringLiteral("열기"), qPrintable(lang));
            if (!expected.isEmpty())
                QCOMPARE(t, expected);
            QVERIFY(!tr.translate("Gifiles", "선택한 항목들로…").isEmpty());
        }
        for (const auto &[lang, expected] : open) {
            QFile ts(dir + QStringLiteral("/gifiles_%1.ts").arg(lang));
            QVERIFY(ts.open(QIODevice::ReadOnly));
            QVERIFY2(!ts.readAll().contains("type=\"unfinished\""), qPrintable(QStringLiteral("untranslated messages in %1").arg(ts.fileName())));
        }
    }


    void humanSize()
    {
        const QLocale saved;
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        QCOMPARE(util::humanSize(0), QStringLiteral("0바이트"));
        QCOMPARE(util::humanSize(999), QStringLiteral("999바이트"));
        QCOMPARE(util::humanSize(1000), QStringLiteral("1.0 KB"));
        QCOMPARE(util::humanSize(1500), QStringLiteral("1.5 KB"));
        QCOMPARE(util::humanSize(12345), QStringLiteral("12 KB"));
        QCOMPARE(util::humanSize(123456), QStringLiteral("123 KB"));
        QCOMPARE(util::humanSize(1200000), QStringLiteral("1.2 MB"));
        QCOMPARE(util::humanSize(5000000000LL), QStringLiteral("5.0 GB"));
        QCOMPARE(util::humanSize(2000000000000LL), QStringLiteral("2.0 TB"));
        QCOMPARE(util::humanSize(3000000000000000LL), QStringLiteral("3.0 PB"));
        QCOMPARE(util::humanSize(5000000000000000000LL), QStringLiteral("5,000 PB")); // no unit past PB
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        QCOMPARE(util::humanSize(1500), QStringLiteral("1,5 KB")); // the user's decimal separator
        // Just below a unit boundary: rounding must not show "10.0 KB" / "1000 KB" (Finder: "10 KB", "1 MB").
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        const QString a = util::humanSize(9999), b = util::humanSize(999999);
        QLocale::setDefault(saved);
        QCOMPARE(a, QStringLiteral("10 KB"));
        QCOMPARE(b, QStringLiteral("1.0 MB"));
    }

    void humanDate()
    {
        QCOMPARE(util::humanDate(QDateTime()), QStringLiteral("--"));
        QCOMPARE(util::humanDate(QDateTime(QDate(2025, 9, 30), QTime(15, 4))), QStringLiteral("2025. 9. 30. 15:04"));
        QCOMPARE(util::humanDate(QDateTime(QDate(2001, 1, 2), QTime(0, 0, 59))), QStringLiteral("2001. 1. 2. 00:00"));
    }

    void kindOf()
    {
        const QString dir = scratch(QStringLiteral("kinds"));
        auto touch = [&](const QString &name) {
            QVERIFY(writeText(QDir(dir).filePath(name), QStringLiteral("x")));
        };
        for (const char *n : {"a.png", "a.TXT", "a.py", "a.mp3", "a.mov", "a.zip", "a.pdf", "a.exe", "README", "a.weird"})
            touch(QString::fromLatin1(n));
        QDir(dir).mkdir(QStringLiteral("folder.png"));
        auto kind = [&](const char *n) { return util::kindOf(QFileInfo(QDir(dir).filePath(QString::fromLatin1(n)))); };
        QCOMPARE(kind("folder.png"), QStringLiteral("폴더"));
        QCOMPARE(kind("a.png"), QStringLiteral("PNG 이미지"));
        QCOMPARE(kind("a.TXT"), QStringLiteral("텍스트"));
        QCOMPARE(kind("a.py"), QStringLiteral("PY 소스"));
        QCOMPARE(kind("a.mp3"), QStringLiteral("MP3 오디오"));
        QCOMPARE(kind("a.mov"), QStringLiteral("MOV 동영상"));
        QCOMPARE(kind("a.zip"), QStringLiteral("ZIP 압축 파일"));
        QCOMPARE(kind("a.pdf"), QStringLiteral("PDF 문서"));
        QCOMPARE(kind("a.exe"), QStringLiteral("실행 파일"));
        QCOMPARE(kind("README"), QStringLiteral("문서"));
        QCOMPARE(kind("a.weird"), QStringLiteral("WEIRD 파일"));
#ifndef Q_OS_WIN
        touch(QStringLiteral("script"));
        QFile::setPermissions(QDir(dir).filePath(QStringLiteral("script")), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QCOMPARE(kind("script"), QStringLiteral("실행 파일"));
#endif
#ifdef Q_OS_MACOS
        QCOMPARE(util::kindOf(QFileInfo(QStringLiteral("/System/Applications/Calculator.app"))), QStringLiteral("응용 프로그램"));
        QVERIFY(util::isPackage(QFileInfo(QStringLiteral("/System/Applications/Calculator.app"))));
#endif
        QVERIFY(!util::isPackage(QFileInfo(dir)));
    }

    void splitExt_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("base");
        QTest::addColumn<QString>("ext");
        QTest::newRow("simple") << "a.txt" << "a" << ".txt";
        QTest::newRow("double") << "a.tar.gz" << "a.tar" << ".gz";
        QTest::newRow("dotfile") << ".bashrc" << ".bashrc" << "";
        QTest::newRow("dotfile ext") << ".env.local" << ".env" << ".local";
        QTest::newRow("none") << "Makefile" << "Makefile" << "";
        QTest::newRow("trailing dot") << "a." << "a" << ".";
        QTest::newRow("empty") << "" << "" << "";
        QTest::newRow("korean") << "한글 파일.한글" << "한글 파일" << ".한글";
    }
    void splitExt()
    {
        QFETCH(QString, name);
        QFETCH(QString, base);
        QFETCH(QString, ext);
        QString b = QStringLiteral("junk"), e = QStringLiteral("junk");
        util::splitExt(name, &b, &e);
        QCOMPARE(b, base);
        QCOMPARE(e, ext);
    }

    void uniqueNames()
    {
        const QString dir = scratch(QStringLiteral("unique"));
        auto touch = [&](const QString &name) { QVERIFY(writeText(QDir(dir).filePath(name), QString())); };
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral("a.txt"), false), QStringLiteral("a.txt"));
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral("a.txt"), true), QStringLiteral("a 복사본.txt"));
        touch(QStringLiteral("a.txt"));
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral("a.txt"), false), QStringLiteral("a 복사본.txt"));
        touch(QStringLiteral("a 복사본.txt"));
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral("a.txt"), false), QStringLiteral("a 복사본 2.txt"));
        touch(QStringLiteral("a 복사본 2.txt"));
        // Copying a copy doesn't nest ("a 복사본 2 복사본").
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral("a 복사본 2.txt"), false), QStringLiteral("a 복사본 3.txt"));
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral("a 복사본.txt"), true), QStringLiteral("a 복사본 3.txt"));
        QDir(dir).mkdir(QStringLiteral("folder"));
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral("folder"), false), QStringLiteral("folder 복사본"));
        touch(QStringLiteral(".env"));
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral(".env"), false), QStringLiteral(".env 복사본"));
        touch(QStringLiteral("x.tar.gz"));
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral("x.tar.gz"), false), QStringLiteral("x.tar 복사본.gz"));

        QCOMPARE(util::uniquePlainName(dir, QStringLiteral("new")), QStringLiteral("new"));
        QCOMPARE(util::uniquePlainName(dir, QStringLiteral("a.txt")), QStringLiteral("a 2.txt"));
        touch(QStringLiteral("a 2.txt"));
        QCOMPARE(util::uniquePlainName(dir, QStringLiteral("a.txt")), QStringLiteral("a 3.txt"));
        QCOMPARE(util::uniquePlainName(dir, QStringLiteral("folder")), QStringLiteral("folder 2"));
#ifndef Q_OS_WIN
        // A dangling symlink takes its name too (exists() doesn't follow links).
        QVERIFY(QFile::link(QDir(dir).filePath(QStringLiteral("missing-target")), QDir(dir).filePath(QStringLiteral("link"))));
        QVERIFY(!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("link"))));
        QVERIFY(util::exists(QDir(dir).filePath(QStringLiteral("link"))));
        QCOMPARE(util::uniquePlainName(dir, QStringLiteral("link")), QStringLiteral("link 2"));
        QCOMPARE(util::uniqueCopyName(dir, QStringLiteral("link"), false), QStringLiteral("link 복사본"));
#endif
    }

    void isInside()
    {
        QVERIFY(util::isInside(QStringLiteral("/a/b"), QStringLiteral("/a")));
        QVERIFY(util::isInside(QStringLiteral("/a/b/c"), QStringLiteral("/a/")));
        QVERIFY(util::isInside(QStringLiteral("/a"), QStringLiteral("/a")));
        QVERIFY(util::isInside(QStringLiteral("/a/"), QStringLiteral("/a")));
        QVERIFY(util::isInside(QStringLiteral("/a"), QStringLiteral("/")));
        QVERIFY(util::isInside(QStringLiteral("/a/x/../b"), QStringLiteral("/a")));
        QVERIFY(!util::isInside(QStringLiteral("/ab"), QStringLiteral("/a")));
        QVERIFY(!util::isInside(QStringLiteral("/a"), QStringLiteral("/a/b")));
        QVERIFY(!util::isInside(QStringLiteral("/a/../b"), QStringLiteral("/a")));
        QVERIFY(!util::isInside(QStringLiteral("/b"), QStringLiteral("/a")));
    }

    void colorAndExtensionHelpers()
    {
        QVERIFY(util::isHexColor(QStringLiteral("#000000")));
        QVERIFY(util::isHexColor(QStringLiteral("#a1B2c3")));
        for (const char *bad : {"", "#", "000000", "#00000", "#0000000", "#GG0000", " #000000", "#000000 ", "rgb(0,0,0)", "#00 000"})
            QVERIFY2(!util::isHexColor(QString::fromLatin1(bad)), bad);
        QCOMPARE(util::extensionList(QStringLiteral("ZIP, .7z tar")), (QStringList{QStringLiteral("zip"), QStringLiteral("7z"), QStringLiteral("tar")}));
        QCOMPARE(util::extensionList(QStringLiteral("zip,ZIP,  .zip\n..tgz\ttar.gz")),
                 (QStringList{QStringLiteral("zip"), QStringLiteral("tgz"), QStringLiteral("tar.gz")}));
        QCOMPARE(util::extensionList(QStringLiteral(" , ,. ")), QStringList());
        QCOMPARE(util::extensionList(QString()), QStringList());
    }

    void validateName()
    {
        QVERIFY(util::validateName(QStringLiteral("ok name.txt")).isEmpty());
        QVERIFY(util::validateName(QStringLiteral("한글")).isEmpty());
        QVERIFY(util::validateName(QStringLiteral(".hidden")).isEmpty());
        QVERIFY(util::validateName(QStringLiteral("...")).isEmpty());
        QCOMPARE(util::validateName(QString()), QStringLiteral("이름이 비어 있습니다."));
        QCOMPARE(util::validateName(QStringLiteral("   ")), QStringLiteral("이름이 비어 있습니다."));
        QCOMPARE(util::validateName(QStringLiteral(".")), QStringLiteral("사용할 수 없는 이름입니다."));
        QCOMPARE(util::validateName(QStringLiteral("..")), QStringLiteral("사용할 수 없는 이름입니다."));
        QVERIFY(!util::validateName(QStringLiteral("a/b")).isEmpty());
#ifdef Q_OS_WIN
        for (const char *bad : {"a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b"})
            QVERIFY2(!util::validateName(QString::fromLatin1(bad)).isEmpty(), bad);
#else
        for (const char *ok : {"a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b"})
            QVERIFY2(util::validateName(QString::fromLatin1(ok)).isEmpty(), ok);
#endif
    }

    void pathHelpers()
    {
        QCOMPARE(util::displayName(QStringLiteral("/a/b/file.txt")), QStringLiteral("file.txt"));
        QVERIFY(!util::displayName(QDir::rootPath()).isEmpty());
        QVERIFY(!util::nativeShortcutText(QStringLiteral("Ctrl+O")).isEmpty());
        QCOMPARE(util::nativeShortcutText(QString()), QString());

        const QString dir = scratch(QStringLiteral("reveal"));
        const QString file = QDir(dir).filePath(QStringLiteral("a,b.txt"));
        QVERIFY(writeText(file, QString()));
        const util::Command forFile = util::revealCommand(file), forDir = util::revealCommand(dir);
#if defined(Q_OS_MACOS)
        QCOMPARE(forFile.program, QStringLiteral("open"));
        QCOMPARE(forFile.args, (QStringList{QStringLiteral("-R"), QFileInfo(file).absoluteFilePath()}));
        QCOMPARE(forDir.args, QStringList{QFileInfo(dir).absoluteFilePath()});
        QCOMPARE(util::revealActionId(), QStringLiteral("Finder에서 열기"));
#elif defined(Q_OS_WIN)
        QCOMPARE(forFile.program, QStringLiteral("explorer.exe"));
        QCOMPARE(forFile.args, QStringList{QStringLiteral("/select,") + QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath())});
        QCOMPARE(forDir.args, QStringList{QDir::toNativeSeparators(QFileInfo(dir).absoluteFilePath())});
        QCOMPARE(util::revealActionId(), QStringLiteral("탐색기에서 열기"));
#else
        QCOMPARE(forFile.program, QStringLiteral("dbus-send"));
        QVERIFY(forFile.args.contains(QStringLiteral("org.freedesktop.FileManager1.ShowItems")));
        QVERIFY(forDir.args.contains(QStringLiteral("org.freedesktop.FileManager1.ShowFolders")));
        // dbus-send splits array items at commas: the comma in the name must be encoded.
        const QString arg = forFile.args.value(forFile.args.size() - 2);
        QVERIFY2(arg.startsWith(QStringLiteral("array:string:file://")) && arg.endsWith(QStringLiteral("/a%2Cb.txt")), qPrintable(arg));
        QCOMPARE(util::revealActionId(), QStringLiteral("파일 관리자에서 열기"));
#endif
        QCOMPARE(util::revealActionText(), util::revealActionId()); // untranslated
        QVERIFY(!util::revealShowText().isEmpty());
    }

    // ---------------------------------------------------------------- natural sorting (FileProxy)

    void naturalSorting_data()
    {
        QTest::addColumn<bool>("cLocale"); // the C/POSIX locale uses the app's own natural compare
        QTest::newRow("C locale") << true;
        QTest::newRow("English") << false;
    }
    void naturalSorting()
    {
        QFETCH(bool, cLocale);
        const QString dir = scratch(QStringLiteral("natural"));
        const QStringList files{QStringLiteral("a10.txt"), QStringLiteral("a2.txt"), QStringLiteral("A1.txt"), QStringLiteral("b.txt"),
                                QStringLiteral("a2b.txt"), QStringLiteral("a002c.txt"), QStringLiteral("a100.txt")};
        for (const QString &f : files)
            QVERIFY(writeText(QDir(dir).filePath(f), QString()));
        QDir(dir).mkdir(QStringLiteral("zz folder"));
        const QLocale saved;
        QLocale::setDefault(cLocale ? QLocale::c() : QLocale(QLocale::English, QLocale::UnitedStates));
        QFileSystemModel fs;
        fs.setRootPath(dir);
        FileProxy proxy(&fs);
        QLocale::setDefault(saved);
        proxy.sort(ColName, Qt::AscendingOrder);
        auto root = [&] { return proxy.mapFromSource(fs.index(dir)); }; // fresh: the proxy's indexes move while it loads
        QTRY_COMPARE(proxy.rowCount(root()), int(files.size()) + 1);
        auto names = [&] {
            QStringList out;
            for (int r = 0; r < proxy.rowCount(root()); ++r)
                out << proxy.index(r, ColName, root()).data().toString();
            return out;
        };
        // Folders first, digit runs by value, case-insensitive. Where "a002c" goes among the 2s
        // (leading zeros) is the platform collator's call: Windows (like Explorer) differs from ICU.
        QStringList expected{QStringLiteral("zz folder"), QStringLiteral("A1.txt"), QStringLiteral("a2.txt"), QStringLiteral("a2b.txt"),
                             QStringLiteral("a002c.txt"), QStringLiteral("a10.txt"), QStringLiteral("a100.txt"), QStringLiteral("b.txt")};
        const QString unsure = [&] {
#ifdef Q_OS_WIN
            if (!cLocale)
                return QStringLiteral("a002c.txt");
#endif
            return QString();
        }();
        expected.removeAll(unsure);
        auto shown = [&] { QStringList l = names(); l.removeAll(unsure); return l; };
        QTRY_COMPARE(shown(), expected);
        proxy.sort(ColName, Qt::DescendingOrder);
        QTRY_COMPARE(names().first(), QStringLiteral("zz folder")); // folders first in both directions
        QCOMPARE(names().last(), QStringLiteral("A1.txt"));
        // Search filters the root's children, case-insensitively.
        proxy.setSearchRoot(dir);
        proxy.setSearch(QStringLiteral("A2"));
        QTRY_COMPARE(proxy.rowCount(root()), 2);
        proxy.setSearch(QString());
        QTRY_COMPARE(proxy.rowCount(root()), int(files.size()) + 1);
    }

    // ---------------------------------------------------------------- command quoting

    void quoteWordExact()
    {
#ifdef Q_OS_WIN
        QCOMPARE(TerminalWidget::quoteWord(QString()), QStringLiteral("\"\""));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("a b")), QStringLiteral("\"a b\""));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("it's")), QStringLiteral("\"it's\""));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("a\"b")), QStringLiteral("\"a`\"b\""));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("$env:x")), QStringLiteral("\"`$env:x\""));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("`")), QStringLiteral("\"``\""));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("C:\\x\\")), QStringLiteral("\"C:\\x\\\""));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("a\nb\rc\td")), QStringLiteral("\"a`nb`rc`td\""));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("\x01\x7f")), QStringLiteral("\"$([char]0x01)$([char]0x7f)\""));
        QCOMPARE(TerminalWidget::quoteWord(QString::fromUtf8("\xe2\x80\x9cq\xe2\x80\x9d\xe2\x80\x9e")),
                 QString::fromUtf8("\"`\xe2\x80\x9cq`\xe2\x80\x9d`\xe2\x80\x9e\""));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("한글")), QStringLiteral("\"한글\""));
#else
        QCOMPARE(TerminalWidget::quoteWord(QString()), QStringLiteral("$''"));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("a b")), QStringLiteral("$'a b'"));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("it's")), QStringLiteral("$'it\\'s'"));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("a\\b")), QStringLiteral("$'a\\\\b'"));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("$x `y` \"z\"")), QStringLiteral("$'$x `y` \"z\"'"));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("a\nb\rc\td")), QStringLiteral("$'a\\nb\\rc\\td'"));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("\x01" "2\x1b\x7f")), QStringLiteral("$'\\x012\\x1b\\x7f'"));
        QCOMPARE(TerminalWidget::quoteWord(QStringLiteral("한글")), QStringLiteral("$'한글'"));
#endif
    }

    // Whatever the text, the quoted word is one line without raw control characters (a raw CR typed
    // into the pty would run the line) and has no unescaped closing quote.
    void quoteWordNeverBreaksOut()
    {
        QRandomGenerator rng(7);
        QStringList inputs = nastyWords();
        for (int i = 0; i < 500; ++i) {
            QString s;
            const int n = rng.bounded(10);
            for (int k = 0; k < n; ++k)
                s += QChar(char16_t(rng.bounded(2) ? rng.bounded(0x80) : rng.bounded(0x2000, 0x2030)));
            inputs << s;
        }
        for (const QString &s : inputs) {
            const QString q = TerminalWidget::quoteWord(s);
            for (const QChar c : q)
                QVERIFY2(c.unicode() >= 0x20 && c.unicode() != 0x7f, qPrintable(q));
#ifdef Q_OS_WIN
            QVERIFY(q.startsWith(QLatin1Char('"')) && q.endsWith(QLatin1Char('"')));
            // Inside, every " (and typographic “ ” „) and $ is escaped by a backtick that is not itself escaped.
            const QString body = q.mid(1, q.size() - 2);
            for (qsizetype i = 0; i < body.size(); ++i) {
                if (body[i] == QLatin1Char('`')) {
                    ++i; // escaped pair
                    continue;
                }
                const char16_t u = body[i].unicode();
                QVERIFY2(u != '"' && u != 0x201C && u != 0x201D && u != 0x201E, qPrintable(q));
                if (u == '$')
                    QVERIFY2(body.mid(i, 10).startsWith(QStringLiteral("$([char]0x")), qPrintable(q));
            }
#else
            QVERIFY(q.startsWith(QStringLiteral("$'")) && q.endsWith(QLatin1Char('\'')));
            const QString body = q.mid(2, q.size() - 3);
            for (qsizetype i = 0; i < body.size(); ++i) {
                if (body[i] == QLatin1Char('\\')) {
                    ++i;
                    continue;
                }
                QVERIFY2(body[i] != QLatin1Char('\''), qPrintable(q));
            }
#endif
        }
    }

    void selectionCommandsQuoteEveryName()
    {
        // Spaces, quotes, $, `, Korean, a newline: each path stays one shell word, on one line.
        const QString dir = QDir::cleanPath(scratch(QStringLiteral("quote-every-name")));
        const QStringList paths = {dir + QStringLiteral("/a b.txt"), dir + QStringLiteral("/한글 '따옴표'.txt"),
                                   dir + QStringLiteral("/x$y`z\n.txt")};
        const QString out = TerminalWidget::expandCommand(QStringLiteral("cmd {names} | {dir} | {prompt} | {nope}"), paths,
                                                          QStringLiteral("요청 \"{files}\""));
        QVERIFY(!out.contains(QLatin1Char('\n')));
#ifdef Q_OS_WIN
        QCOMPARE(out, QStringLiteral("cmd \"a b.txt\", \"한글 'Tick'.txt\", \"x`$y``z`n.txt\" | \"%1\" | \"요청 `\"{files}`\"\" | {nope}")
                          .arg(QDir::toNativeSeparators(dir)).replace(QStringLiteral("Tick"), QStringLiteral("따옴표")));
#else
        QCOMPARE(out, QStringLiteral("cmd $'a b.txt' $'한글 \\'따옴표\\'.txt' $'x$y`z\\n.txt' | $'%1' | $'요청 \"{files}\"' | {nope}").arg(dir));
#endif
    }

    void terminalResizeKeepsCursorInRange()
    {
        // A wrapped line's final row contains the cursor when the whole line is
        // too tall for the resized screen. libvterm 0.3.3 used to abort here.
        VTerm *vt = vterm_new(3, 80);
        VTermScreen *screen = vterm_obtain_screen(vt);
        vterm_screen_enable_reflow(screen, true);
        vterm_screen_reset(screen, 1);
        const QByteArray text(200, 'x');
        vterm_input_write(vt, text.constData(), size_t(text.size()));
        vterm_set_size(vt, 2, 10);
        const QByteArray next("\r\nOK");
        vterm_input_write(vt, next.constData(), size_t(next.size()));
        VTermPos cursor;
        vterm_state_get_cursorpos(vterm_obtain_state(vt), &cursor);
        VTermScreenCell cell;
        const bool read = vterm_screen_get_cell(screen, VTermPos{cursor.row, 1}, &cell);
        vterm_free(vt);
        QVERIFY(cursor.row >= 0 && cursor.row < 2);
        QCOMPARE(cursor.col, 2);
        QVERIFY(read);
        QCOMPARE(cell.chars[0], uint32_t('K'));
    }

    void expandCommandPlaceholders()
    {
        const QString dir = scratch(QStringLiteral("expand"));
        const QString a = QDir(dir).filePath(QStringLiteral("a b.txt")), sub = QDir(dir).filePath(QStringLiteral("sub/c.txt"));
        auto q = [](const QString &s) { return TerminalWidget::quoteWord(QDir::toNativeSeparators(s)); };
#ifdef Q_OS_WIN
        const QString sep = QStringLiteral(", ");
#else
        const QString sep = QStringLiteral(" ");
#endif
        QCOMPARE(TerminalWidget::expandCommand(QStringLiteral("x {files} | {names} | {dir} | {prompt} | {other} {FILES} {files"),
                                               {a, sub}, QStringLiteral("p q")),
                 QStringLiteral("x ") + q(a) + sep + q(sub) + QStringLiteral(" | ") + q(QStringLiteral("a b.txt")) + sep +
                     q(QStringLiteral("sub/c.txt")) + QStringLiteral(" | ") + q(dir) + QStringLiteral(" | ") +
                     TerminalWidget::quoteWord(QStringLiteral("p q")) + QStringLiteral(" | {other} {FILES} {files"));
        // {dir} is the first item's folder; {names} are relative to it.
        QCOMPARE(TerminalWidget::expandCommand(QStringLiteral("{dir} {names}"), {sub, a}, QString()),
                 q(QDir(dir).filePath(QStringLiteral("sub"))) + QLatin1Char(' ') + q(QStringLiteral("c.txt")) + sep + q(QStringLiteral("../a b.txt")));
        // Nothing selected: {dir} is the folder shown, the lists are empty.
        QCOMPARE(TerminalWidget::expandCommand(QStringLiteral("cd {dir} && ls {files}{names}."), {}, QString(), dir),
                 QStringLiteral("cd ") + q(dir) + QStringLiteral(" && ls ."));
        QCOMPARE(TerminalWidget::expandCommand(QStringLiteral("{dir}"), {}, QString()), TerminalWidget::quoteWord(QString()));
        // One pass: values that contain placeholders are not expanded again.
        const QString tricky = QDir(dir).filePath(QStringLiteral("{names}{dir}"));
        QCOMPARE(TerminalWidget::expandCommand(QStringLiteral("{files} {prompt}"), {tricky}, QStringLiteral("{files}")),
                 q(tricky) + QLatin1Char(' ') + TerminalWidget::quoteWord(QStringLiteral("{files}")));
        // A name that looks like an option goes in as ./-m (.\-m on Windows), never as a bare -m; {files} are absolute.
        const QString dash = QDir(dir).filePath(QStringLiteral("-m"));
        QCOMPARE(TerminalWidget::expandCommand(QStringLiteral("zip x {names} {files}"), {dash}, QString()),
                 QStringLiteral("zip x ") + q(QStringLiteral("./-m")) + QLatin1Char(' ') + q(dash));
#ifdef Q_OS_WIN
        QVERIFY(TerminalWidget::expandCommand(QStringLiteral("{names}"), {dash}, QString()).contains(QStringLiteral(".\\-m")));
#endif
        QCOMPARE(TerminalWidget::expandCommand(QStringLiteral("no placeholders"), {a}, QStringLiteral("p")), QStringLiteral("no placeholders"));
        QCOMPARE(TerminalWidget::expandCommand(QString(), {a}, QStringLiteral("p")), QString());
    }

#ifndef Q_OS_WIN
    // The real shells read every quoted word back exactly, and nothing in it runs.
    void quotingRoundTripInShells()
    {
        const QString dir = scratch(QStringLiteral("shells"));
        QStringList shells;
        for (const char *name : {"bash", "zsh"})
            if (const QString path = QStandardPaths::findExecutable(QString::fromLatin1(name)); !path.isEmpty())
                shells << path;
        if (shells.isEmpty())
            QSKIP("neither bash nor zsh is installed");
        const QStringList words = nastyWords();
        QStringList quoted;
        for (const QString &w : words)
            quoted << TerminalWidget::quoteWord(w);
        for (const QString &shell : shells) {
            int code = -1;
            const QStringList got = runShellWords(shell, QStringLiteral("printf '%s\\0' ") + quoted.join(QLatin1Char(' ')), dir, &code);
            QCOMPARE(code, 0);
            QCOMPARE(got, words);
            QVERIFY2(QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty(),
                     qPrintable(shell + QStringLiteral(" ran something: ") + QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).join(QLatin1Char(','))));
        }
    }

    // expandCommand with real files whose names try every trick: each placeholder gives the shell exactly the paths.
    void expandCommandInShell()
    {
        const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
        if (bash.isEmpty())
            QSKIP("bash is not installed");
        const QString dir = scratch(QStringLiteral("expand-shell"));
        QStringList paths, names;
        for (const QString &w : nastyWords()) {
            if (w.isEmpty() || w.contains(QLatin1Char('/')) || w == QStringLiteral("--") || w == QStringLiteral("-n"))
                continue;
            paths << QDir(dir).filePath(w);
            names << (w.startsWith(QLatin1Char('-')) ? QStringLiteral("./") + w : w); // never a bare option
        }
        const QString prompt = QStringLiteral("요청 'x' \"y\" $(touch PWNED) `touch PWNED`\nnext %s \\");
        int code = -1;
        const QStringList got = runShellWords(bash, TerminalWidget::expandCommand(QStringLiteral("printf '%s\\0' {files} {names} {dir} {prompt}"), paths, prompt),
                                              m_root.path(), &code);
        QCOMPARE(code, 0);
        QCOMPARE(got, paths + names + (QStringList{dir, prompt}));
        QVERIFY(!QFileInfo::exists(m_root.filePath(QStringLiteral("PWNED"))));
    }

    // The built-in "AI로 실행" command hands the prompt and the files over intact (claude replaced by printf).
    void builtinAiCommandQuoting()
    {
        const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
        if (bash.isEmpty())
            QSKIP("bash is not installed");
        const QString dir = scratch(QStringLiteral("ai"));
        const QString tmpl = Settings::instance()->selectionCommand(QStringLiteral("ai")).value(QStringLiteral("command")).toString();
        QVERIFY(tmpl.contains(QStringLiteral("claude -p ")));
        const QStringList paths{QDir(dir).filePath(QStringLiteral("it's $(x).txt")), QDir(dir).filePath(QStringLiteral("-rf"))};
        const QString prompt = QStringLiteral("summarize \"these\" 100% `now`");
        QString cmd = TerminalWidget::expandCommand(tmpl, paths, prompt);
        cmd.replace(QStringLiteral("claude -p "), QStringLiteral("printf '%s' "));
        int code = -1;
        const QString out = QString::fromUtf8(runShell(bash, cmd, m_root.path(), &code));
        QCOMPARE(code, 0);
        QCOMPARE(out, prompt + QStringLiteral("\n\n대상 파일:\n") + paths.join(QLatin1Char('\n')));
    }

    // The built-in "새로운 폴더" command moves every selected item, whatever its name, into a new folder.
    void builtinNewFolderCommand()
    {
        const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
        if (bash.isEmpty())
            QSKIP("bash is not installed");
        const QString dir = scratch(QStringLiteral("new-folder"));
        QStringList paths;
        for (const char *n : {"-m", "--help", "it's", "a b", "$(touch PWNED)", "line\nbreak"}) {
            paths << QDir(dir).filePath(QString::fromUtf8(n));
            QVERIFY(writeText(paths.last(), QString::fromUtf8(n)));
        }
        const QString tmpl = Settings::instance()->selectionCommand(QStringLiteral("new_folder")).value(QStringLiteral("command")).toString();
        int code = -1;
        runShell(bash, TerminalWidget::expandCommand(tmpl, paths, QString()), m_root.path(), &code);
        QCOMPARE(code, 0);
        const QString folder = QDir(dir).filePath(QStringLiteral("새 폴더"));
        QStringList inside = QDir(folder).entryList(QDir::Files | QDir::Hidden);
        inside.sort();
        QStringList expected;
        for (const QString &path : paths)
            expected << QFileInfo(path).fileName();
        expected.sort();
        QCOMPARE(inside, expected);
        QCOMPARE(QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{QStringLiteral("새 폴더")});
    }

    // The built-in "압축 파일 생성" command: a selected file named like an option must not act as one
    // (zip's -m would delete the originals after zipping them, -T -TT runs a command).
    void builtinZipCommandTreatsNamesAsFiles()
    {
        const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
        if (bash.isEmpty() || QStandardPaths::findExecutable(QStringLiteral("zip")).isEmpty())
            QSKIP("bash or zip is not installed");
        const QString dir = scratch(QStringLiteral("zip"));
        const QStringList paths{QDir(dir).filePath(QStringLiteral("-m")), QDir(dir).filePath(QStringLiteral("a.txt"))};
        for (const QString &p : paths)
            QVERIFY(writeText(p, QStringLiteral("data")));
        const QString tmpl = Settings::instance()->selectionCommand(QStringLiteral("zip")).value(QStringLiteral("command")).toString();
        int code = -1;
        runShell(bash, TerminalWidget::expandCommand(tmpl, paths, QString()), m_root.path(), &code);
        QCOMPARE(code, 0);
        QVERIFY(QFileInfo::exists(paths[0]));
        QVERIFY(QFileInfo::exists(paths[1])); // "-m" didn't act as zip's move option
        QVERIFY(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("압축 파일.zip"))));
    }
#else
    // PowerShell reads every quoted word back exactly (run as runQuietly does, -EncodedCommand).
    void quotingRoundTripInPowerShell()
    {
        const QString dir = scratch(QStringLiteral("ps"));
        QStringList paths, expected;
        for (const QString &w : nastyWords()) {
            if (w.isEmpty())
                continue;
            paths << QDir(dir).filePath(w);
            expected << hexUnits(QDir::toNativeSeparators(paths.last()));
        }
        const QString prompt = QString::fromUtf8("요청 'x' \"y\" $(New-Item PWNED) `n \xe2\x80\x9cq\xe2\x80\x9d\nnext");
        expected << hexUnits(prompt) << hexUnits(QDir::toNativeSeparators(dir));
        const QString script = TerminalWidget::expandCommand(
            QStringLiteral("Set-Location -LiteralPath {dir}; foreach ($a in @({files}, {prompt}, {dir})) { "
                           "[Console]::Out.WriteLine((($a.ToCharArray() | ForEach-Object { '{0:x4}' -f [int]$_ }) -join ' ')) }"),
            paths, prompt);
        QCOMPARE(runPowerShell(script), expected);
        QVERIFY(!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("PWNED"))));
    }
#endif

    // ---------------------------------------------------------------- the command line (main.cpp)

    void cliHelp()
    {
        const QString dir = scratch(QStringLiteral("cli-help"));
        QVERIFY(writeText(QDir(dir).filePath(QStringLiteral("config.toml")), QStringLiteral("[general]\nlanguage = \"ko\"\n")));
        const Run help = runApp({QStringLiteral("--help")}, dir);
        QCOMPARE(help.code, 0);
        QVERIFY(help.err.isEmpty());
        for (const char *option : {"--config-path", "--print-config", "--print-default-config", "--check-config", "GIFILES_CONFIG_DIR"})
            QVERIFY2(help.out.contains(QLatin1String(option)), option);
        QVERIFY(help.out.startsWith(QStringLiteral("Gifiles - 터미널이 붙은 파일 관리자\n")));
        const Run h = runApp({QStringLiteral("-h")}, dir);
        QCOMPARE(h.code, 0);
        QCOMPARE(h.out, help.out);
        // The language setting applies to the command line too.
        QVERIFY(writeText(QDir(dir).filePath(QStringLiteral("config.toml")), QStringLiteral("[general]\nlanguage = \"en\"\n")));
        const Run en = runApp({QStringLiteral("--help")}, dir);
        QCOMPARE(en.code, 0);
        QVERIFY(en.out.contains(QStringLiteral("--check-config")));
        QVERIFY2(!en.out.contains(QStringLiteral("터미널")), qPrintable(en.out));
    }

    void cliConfigPath()
    {
        const QString dir = m_root.filePath(QStringLiteral("cli-path/not yet"));
        const Run r = runApp({QStringLiteral("--config-path")}, dir);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.out, QDir::toNativeSeparators(QDir::cleanPath(dir + QStringLiteral("/config.toml"))) + QLatin1Char('\n'));
        QVERIFY(r.err.isEmpty());
        QVERIFY(!QFileInfo::exists(dir)); // printing the path creates nothing
    }

    void cliPrintDefaultConfig()
    {
        const QString dir = scratch(QStringLiteral("cli-defaults"));
        QVERIFY(writeText(QDir(dir).filePath(QStringLiteral("config.toml")), QStringLiteral("[general]\nlanguage = \"ko\"\n[view]\nicon_size = 200\n")));
        const Run r = runApp({QStringLiteral("--print-default-config")}, dir);
        QCOMPARE(r.code, 0);
        QVERIFY(r.err.isEmpty());
        QCOMPARE(r.out, Settings::renderDefaults()); // the file's own values don't matter
        QVERIFY(parseOk(r.out));
        // Its output is a valid config.toml for --check-config.
        const QString out = m_root.filePath(QStringLiteral("cli-defaults-out.toml"));
        QVERIFY(writeText(out, r.out));
        const Run check = runApp({QStringLiteral("--check-config"), out}, dir);
        QCOMPARE(check.code, 0);
        QCOMPARE(check.out, QDir::toNativeSeparators(out) + QStringLiteral(": 문제 없음\n"));
        // Without a config file in a fresh folder: nothing is created.
        const QString fresh = m_root.filePath(QStringLiteral("cli-defaults-fresh"));
        const Run f = runApp({QStringLiteral("--print-default-config")}, fresh);
        QCOMPARE(f.code, 0);
        QVERIFY(parseOk(f.out));
        QVERIFY(Settings::check(f.out).isEmpty());
        QVERIFY(!QFileInfo::exists(fresh));
    }

    void cliPrintConfig()
    {
        const QString dir = scratch(QStringLiteral("cli-print"));
        const QString file = QDir(dir).filePath(QStringLiteral("config.toml"));
        QVERIFY(writeText(file, QStringLiteral("[general]\nlanguage = \"ko\"\n[view]\nstripes = false\nicon_size = 9999\n"
                                               "[open_with]\nmd = \"/x/Ed.app\"\n[shortcuts]\n\"열기\" = [\"F3\"]\n")));
        const Run r = runApp({QStringLiteral("--print-config")}, dir);
        QCOMPARE(r.code, 0);
        QVERIFY(r.out.contains(QStringLiteral("\nstripes = false\n")));
        QVERIFY(r.out.contains(QStringLiteral("\nicon_size = 96\n"))); // the wrong value: its default
        QVERIFY(r.out.contains(QStringLiteral("\nlanguage = \"ko\"\n")));
        QVERIFY(r.out.contains(QStringLiteral("\nmd = \"/x/Ed.app\"\n")));
        QVERIFY(r.out.contains(QStringLiteral("\n\"열기\" = [\"F3\"]")));
        QVERIFY(Settings::check(r.out).isEmpty());
        QCOMPARE(r.err.count(QLatin1Char('\n')), 1); // the problem, on stderr only
        QVERIFY2(r.err.startsWith(QStringLiteral("config.toml: view.icon_size: 32..256")), qPrintable(r.err));
        QVERIFY(!r.out.contains(QStringLiteral("32..256 사이의")));

        // A syntax error: the defaults, and the error on stderr.
        QVERIFY(writeText(file, QStringLiteral("[view]\nstripes = = false\n")));
        const Run broken = runApp({QStringLiteral("--print-config")}, dir);
        QCOMPARE(broken.code, 0);
        QVERIFY(broken.out.contains(QStringLiteral("\nstripes = true\n")));
        // A broken file can't give its language: the message is in the system's (Korean or English).
        QVERIFY2(broken.err.startsWith(QStringLiteral("config.toml: 2번째 줄")) || broken.err.startsWith(QStringLiteral("config.toml: Line 2")),
                 qPrintable(broken.err));

        // No file: the defaults; the file is not created.
        const QString fresh = m_root.filePath(QStringLiteral("cli-print-fresh"));
        const Run none = runApp({QStringLiteral("--print-config")}, fresh);
        QCOMPARE(none.code, 0);
        QVERIFY(none.err.isEmpty());
        QVERIFY(parseOk(none.out));
        QVERIFY(Settings::check(none.out).isEmpty());
        QVERIFY(!QFileInfo::exists(fresh));
    }

    void cliCheckConfig()
    {
        const QString dir = scratch(QStringLiteral("cli-check"));
        const QString file = QDir(dir).filePath(QStringLiteral("config.toml"));
        const QString native = QDir::toNativeSeparators(file);

        // The configured file, fine.
        QVERIFY(writeText(file, QStringLiteral("[general]\nlanguage = \"ko\"\n[view]\nicon_size = 64\n")));
        Run r = runApp({QStringLiteral("--check-config")}, dir);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.out, native + QStringLiteral(": 문제 없음\n"));

        // Problems: listed, exit 1.
        QVERIFY(writeText(file, QStringLiteral("[general]\nlanguage = \"ko\"\n[view]\nicon_size = 1\nzoom = 2\n")));
        r = runApp({QStringLiteral("--check-config")}, dir);
        QCOMPARE(r.code, 1);
        const QStringList lines = r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 3);
        QCOMPARE(lines[0], native + QLatin1Char(':'));
        QVERIFY(lines[1].startsWith(QStringLiteral("  view.icon_size: 32..256")));
        QVERIFY(lines[2].startsWith(QStringLiteral("  view.zoom: 알 수 없는 항목")));

        // A syntax error.
        QVERIFY(writeText(file, QStringLiteral("[general]\nlanguage = \"ko\"\n[view\n")));
        r = runApp({QStringLiteral("--check-config")}, dir);
        QCOMPARE(r.code, 1);
        QVERIFY2(r.out.contains(QStringLiteral("  3번째 줄: ")) || r.out.contains(QStringLiteral("  Line 3: ")), qPrintable(r.out)); // system language

        // Another file given on the command line; the configured one stays as it is.
        QVERIFY(writeText(file, QStringLiteral("[general]\nlanguage = \"ko\"\n")));
        const QString other = m_root.filePath(QStringLiteral("cli-check-other.toml"));
        QVERIFY(writeText(other, QStringLiteral("[sidebar]\nfavorites = 3\n")));
        r = runApp({QStringLiteral("--check-config"), other}, dir);
        QCOMPARE(r.code, 1);
        QVERIFY(r.out.startsWith(QDir::toNativeSeparators(other) + QStringLiteral(":\n  sidebar.favorites")));

        // Unreadable: a missing file (exit 1, nothing created).
        const QString missing = m_root.filePath(QStringLiteral("cli-check-missing/config.toml"));
        r = runApp({QStringLiteral("--check-config"), missing}, dir);
        QCOMPARE(r.code, 1);
        QVERIFY2(r.out.startsWith(QDir::toNativeSeparators(missing) + QStringLiteral(": 읽을 수 없습니다")), qPrintable(r.out));
        QVERIFY(!QFileInfo::exists(missing));
        // The configured file missing.
        const QString fresh = m_root.filePath(QStringLiteral("cli-check-fresh"));
        r = runApp({QStringLiteral("--check-config")}, fresh);
        QCOMPARE(r.code, 1);
        QVERIFY(!QFileInfo::exists(fresh));
        // A folder is not a readable file.
        r = runApp({QStringLiteral("--check-config"), dir}, dir);
        QCOMPARE(r.code, 1);
    }

    // The folder tree's index (`): what a scan lists, leaves out and how it sorts.
    // The real app starts headless on a folder, draws its views, terminal, settings window and folder
    // tree (GIFILES_SNAPSHOT saves them) and quits cleanly — main()'s whole GUI path, with
    // GIFILES_CONFIG_DIR keeping config.toml and the app's state (QSettings, folders.ini) in the scratch folder.
    void appStartsDrawsAndQuits()
    {
        const QString base = scratch(QStringLiteral("launch"));
        const QString files = QDir(base).filePath(QStringLiteral("files")), config = QDir(base).filePath(QStringLiteral("config")),
                      shots = QDir(base).filePath(QStringLiteral("shots"));
        QVERIFY(QDir().mkpath(files + QStringLiteral("/sub/deeper")));
        QVERIFY(writeText(files + QStringLiteral("/notes.txt"), QStringLiteral("hello\n")));
        QString toml = QStringLiteral("[folder_tree]\nroots = [\"%1\"]\n").arg(files); // never the whole drive
#ifndef Q_OS_WIN
        toml += QStringLiteral("[terminal]\nshell = \"/bin/sh\"\n"); // no user rc files
#endif
        QVERIFY(writeText(config + QStringLiteral("/config.toml"), toml));
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GIFILES_CONFIG_DIR"), config);
        env.insert(QStringLiteral("GIFILES_SNAPSHOT"), shots);
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(QStringLiteral(GIFILES_APP_EXE), {files});
        QVERIFY(p.waitForStarted());
        if (!p.waitForFinished(120000)) {
            p.kill();
            p.waitForFinished();
            QFAIL("the app didn't quit");
        }
        const QByteArray out = p.readAll();
        QVERIFY2(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0, out.constData());
        for (const char *name : {"list", "gallery", "columns", "list-rename", "terminal", "settings-shortcuts", "folder-tree"}) {
            const QImage img(QDir(shots).filePath(QString::fromLatin1(name) + QStringLiteral(".png")));
            QVERIFY2(!img.isNull() && img.width() > 400 && img.height() > 300, name);
        }
        // Its state went beside its config.toml, not to the user's own.
        QDirIterator ini(config + QStringLiteral("/state"), {QStringLiteral("*.ini")}, QDir::Files, QDirIterator::Subdirectories);
        QVERIFY(ini.hasNext());
        // Run from this checkout, it keeps the debug log — beside its config, not in the checkout's logs/.
        if (QFileInfo::exists(QStringLiteral(GIFILES_SOURCE_DIR "/CMakeLists.txt"))) {
            const QStringList logs = QDir(config + QStringLiteral("/logs")).entryList({QStringLiteral("gifiles-*.log")});
            QCOMPARE(logs.size(), 1);
            QFile log(config + QStringLiteral("/logs/") + logs.first());
            QVERIFY(log.open(QIODevice::ReadOnly));
            const QByteArray text = log.readAll();
            QVERIFY(text.contains("[app] start"));
            QVERIFY(text.contains("[app] quit"));
        }
    }

    // The folder tree ranks folders the browser showed often; past 1000 it keeps the most shown
    // four fifths (equal counts too: a count threshold once dropped every folder shown just once).
    void folderTreeVisitsAreTrimmedByRank()
    {
        FolderTree *t = FolderTree::instance();
        const QString base = QStringLiteral("/visits-test");
        for (int i = 0; i < 5; ++i)
            for (int n = 0; n <= i; ++n)
                t->noteVisit(base + QStringLiteral("/often%1").arg(i));
        for (int i = 0; i < 1001 - 5; ++i) // the 1001st folder trims
            t->noteVisit(base + QStringLiteral("/once%1").arg(i));
        const QHash<QString, int> v = t->visits();
        QCOMPARE(v.size(), 800);
        for (int i = 0; i < 5; ++i)
            QCOMPARE(v.value(base + QStringLiteral("/often%1").arg(i)), i + 1); // the most shown are kept
        QCOMPARE(std::count_if(v.keyBegin(), v.keyEnd(), [&](const QString &k) { return k.contains(QStringLiteral("/once")); }), 795);
        t->noteVisit(base + QStringLiteral("/latest"));
        QCOMPARE(t->visits().value(base + QStringLiteral("/latest")), 1);
    }

    void folderIndexScan()
    {
        const QString root = QDir::fromNativeSeparators(scratch(QStringLiteral("folder-index")));
        for (const char *d : {"proj/b10", "proj/b2", "proj/node_modules/pkg", "proj/.git/objects", "skip/inside",
                              "keep/Tool.app/Contents", "keep/notes.tmp", "keep/deep/er/est"})
            QVERIFY(QDir().mkpath(root + QLatin1Char('/') + QString::fromUtf8(d)));
        QVERIFY(writeText(root + QStringLiteral("/proj/file.txt"), QStringLiteral("not a folder")));
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(root + QStringLiteral("/proj"), root + QStringLiteral("/keep/link"))); // not followed
#endif
        FolderIndex::Options o;
        o.roots = {root + QLatin1Char('/')};
        o.exclude = {QStringLiteral("node_modules"), QStringLiteral(".GIT"), QStringLiteral("~nothing"),
                     root + QStringLiteral("/skip"), QStringLiteral("*.tmp")};
        o.skipPackages = true;
        std::atomic<int> progress = 0;
        const FolderIndex idx = FolderIndex::scan(o, &progress);
        QCOMPARE(idx.rootCount(), 1);
        QCOMPARE(idx.name(0), root); // the trailing slash dropped
        QStringList all;
        for (int n = 1; n < idx.size(); ++n)
            all << idx.path(n).mid(root.size() + 1);
        // Breadth first, each folder's children together in natural order (b2 before b10).
        QCOMPARE(all, (QStringList{QStringLiteral("keep"), QStringLiteral("proj"), QStringLiteral("keep/deep"),
                                   QStringLiteral("proj/b2"), QStringLiteral("proj/b10"), QStringLiteral("keep/deep/er"),
                                   QStringLiteral("keep/deep/er/est")}));
        QCOMPARE(progress.load(), idx.size());
        QCOMPARE(idx.depth(idx.find(root + QStringLiteral("/keep/deep/er"))), 3);
        QCOMPARE(idx.childCount(0), 2);
        QCOMPARE(idx.find(root + QStringLiteral("/proj/b10/")), idx.find(root + QStringLiteral("/proj/b10")));
        QVERIFY(idx.find(root + QStringLiteral("/proj/b10")) > 0);
        QCOMPARE(idx.find(root + QStringLiteral("/skip")), -1);
        QCOMPARE(idx.findNearest(root + QStringLiteral("/proj/b10/gone/away")), idx.find(root + QStringLiteral("/proj/b10")));
        QCOMPARE(idx.findNearest(QStringLiteral("/elsewhere")), -1);
        QCOMPARE(idx.parent(idx.find(root + QStringLiteral("/proj/b2"))), idx.find(root + QStringLiteral("/proj")));

        // Cancelled: nothing.
        const std::atomic<bool> cancel = true;
        QCOMPARE(FolderIndex::scan(o, nullptr, &cancel).size(), 0);
    }

    // FSEvents' changed folders brought into an index: only those are read again, the rest copied.
    void folderIndexUpdate()
    {
        const QString root = QDir::fromNativeSeparators(scratch(QStringLiteral("folder-update")));
        for (const char *d : {"a/x", "a/y/inside", "b/z", "c"})
            QVERIFY(QDir().mkpath(root + QLatin1Char('/') + QString::fromUtf8(d)));
        FolderIndex::Options o;
        o.roots = {root};
        auto all = [&](const FolderIndex &idx) {
            QStringList out;
            for (int n = idx.rootCount(); n < idx.size(); ++n)
                out << idx.path(n).mid(root.size() + 1);
            out.sort();
            return out;
        };
        const FolderIndex old = FolderIndex::scan(o);
        QVERIFY(QDir().mkpath(root + QStringLiteral("/a/new/deep/er"))); // a new folder comes with all inside
        QVERIFY(QDir(root + QStringLiteral("/a/y")).removeRecursively());
        QVERIFY(QDir().rename(root + QStringLiteral("/b/z"), root + QStringLiteral("/b/w")));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/c/q"))); // c isn't reported: stays as it was
        bool differs = false;
        const FolderIndex next = FolderIndex::update(old, o, {root + QStringLiteral("/a"), root + QStringLiteral("/b/"),
                                                              root + QStringLiteral("/not/there")}, {}, nullptr, nullptr, &differs);
        QVERIFY(differs);
        QCOMPARE(all(next), (QStringList{QStringLiteral("a"), QStringLiteral("a/new"), QStringLiteral("a/new/deep"),
                                         QStringLiteral("a/new/deep/er"), QStringLiteral("a/x"), QStringLiteral("b"),
                                         QStringLiteral("b/w"), QStringLiteral("c")}));
        QCOMPARE(next.parent(next.find(root + QStringLiteral("/a/new/deep"))), next.find(root + QStringLiteral("/a/new")));
        // Nothing changed in the folders reported: the same index.
        FolderIndex::update(next, o, {root + QStringLiteral("/a")}, {}, nullptr, nullptr, &differs);
        QVERIFY(!differs);
        // `deep`: everything inside read again, like a scan.
        QCOMPARE(all(FolderIndex::update(next, o, {}, {root}, nullptr, nullptr, &differs)), all(FolderIndex::scan(o)));
        QVERIFY(differs);
        // Other roots: a full scan.
        FolderIndex::Options other = o;
        other.roots = {root + QStringLiteral("/a")};
        QCOMPARE(FolderIndex::update(old, other, {}, {}).size(), FolderIndex::scan(other).size());
        // The journal point is kept with the cache.
        const QString file = m_root.filePath(QStringLiteral("folder-update.bin"));
        QVERIFY(next.save(file, QStringLiteral("key"), 1, {42, QStringLiteral("uuid")}));
        FolderIndex back;
        FolderIndex::Journal journal;
        QVERIFY(FolderIndex::load(file, QStringLiteral("key"), back, nullptr, &journal));
        QCOMPARE(journal.eventId, quint64(42));
        QCOMPARE(journal.id, QStringLiteral("uuid"));
    }

    void folderIndexMatch()
    {
        const QString root = QDir::fromNativeSeparators(scratch(QStringLiteral("folder-match")));
        const QString hangul = QStringLiteral("한글폴더").normalized(QString::NormalizationForm_D); // as macOS names it
        for (const QString &d : {QStringLiteral("Sites/gifiles/src"), QStringLiteral("Sites/big gift"), QStringLiteral("Sites/legif"),
                                 QStringLiteral("gif-tools"), QStringLiteral("work/gif"), QStringLiteral("work/zz/GIF"),
                                 QStringLiteral("docs/") + hangul, QStringLiteral("a/x/src"), QStringLiteral("b/src")})
            QVERIFY(QDir().mkpath(root + QLatin1Char('/') + d));
        FolderIndex::Options o;
        o.roots = {root};
        const FolderIndex idx = FolderIndex::scan(o);
        auto paths = [&](const FolderIndex::Matches &m) {
            QStringList out;
            for (int n : m.best)
                out << idx.path(n).mid(root.size() + 1);
            return out;
        };
        // The query's characters in order anywhere in the path (slashes ignored). Best first: name
        // equal, name prefix, inside the name, ...; the folders inside a match last.
        const QStringList gif = {QStringLiteral("work/gif"), QStringLiteral("work/zz/GIF"), QStringLiteral("gif-tools"),
                                 QStringLiteral("Sites/gifiles"), QStringLiteral("Sites/big gift"), QStringLiteral("Sites/legif"),
                                 QStringLiteral("Sites/gifiles/src")};
        QCOMPARE(paths(idx.match(QStringLiteral("gif"))), gif);
        QCOMPARE(idx.match(QStringLiteral("gif")).total, 7);
        QCOMPARE(paths(idx.match(QStringLiteral("GIF"), {}, 2000, true)), QStringList{QStringLiteral("work/zz/GIF")});
        // Visits move a folder up among the same kind of match.
        QHash<int, int> boost;
        boost.insert(idx.find(root + QStringLiteral("/work/zz/GIF")), 3);
        QCOMPARE(paths(idx.match(QStringLiteral("GIF"), boost)).first(), QStringLiteral("work/zz/GIF"));
        // Across folders: "sigif" is S-I-tes + GIF-iles; a slash typed is ignored.
        QStringList sigif = paths(idx.match(QStringLiteral("sigif")));
        QCOMPARE(sigif, paths(idx.match(QStringLiteral("si/gif"))));
        QCOMPARE(sigif.size(), 4);
        QCOMPARE(sigif.last(), QStringLiteral("Sites/gifiles/src"));
        sigif.removeLast();
        sigif.sort();
        QCOMPARE(sigif, (QStringList{QStringLiteral("Sites/big gift"), QStringLiteral("Sites/gifiles"), QStringLiteral("Sites/legif")}));
        QCOMPARE(paths(idx.match(QStringLiteral("gifsrc"))), QStringList{QStringLiteral("Sites/gifiles/src")});
        // Equal matches: the nearest to the browser's folder first, else the shallowest.
        const QStringList srcs = {QStringLiteral("b/src"), QStringLiteral("a/x/src"), QStringLiteral("Sites/gifiles/src")};
        QCOMPARE(paths(idx.match(QStringLiteral("src"))), srcs);
        QCOMPARE(paths(idx.match(QStringLiteral("src"), {}, 2000, false, idx.find(root + QStringLiteral("/Sites/gifiles")))).first(),
                 QStringLiteral("Sites/gifiles/src"));
        QCOMPARE(paths(idx.match(QStringLiteral("src"), {}, 2000, false, idx.find(root + QStringLiteral("/a/x")))),
                 (QStringList{QStringLiteral("a/x/src"), QStringLiteral("b/src"), QStringLiteral("Sites/gifiles/src")}));
        QCOMPARE(paths(idx.match(QStringLiteral("zzwork"))), QStringList()); // in order only
        // Decomposed Hangul matches what is typed (composed).
        QCOMPARE(paths(idx.match(QStringLiteral("한글"))).value(0).normalized(QString::NormalizationForm_C),
                 QStringLiteral("docs/한글폴더"));
        QCOMPARE(idx.match(QStringLiteral("  ")).total, 0);
        QCOMPARE(int(idx.match(QStringLiteral("gif"), {}, 2).best.size()), 2);
        QCOMPARE(idx.match(QStringLiteral("gif"), {}, 2).total, 7);

        // The cache file: back as it was; another key, a damaged or missing file is refused.
        const QString file = m_root.filePath(QStringLiteral("folder-match.bin"));
        QVERIFY(idx.save(file, QStringLiteral("key"), 1234));
        FolderIndex back;
        qint64 at = 0;
        QVERIFY(FolderIndex::load(file, QStringLiteral("key"), back, &at));
        QCOMPARE(at, 1234);
        QCOMPARE(back.size(), idx.size());
        QCOMPARE(back.path(back.size() - 1), idx.path(idx.size() - 1));
        QCOMPARE(paths(back.match(QStringLiteral("gif"))), paths(idx.match(QStringLiteral("gif"))));
        QVERIFY(!FolderIndex::load(file, QStringLiteral("other key"), back));
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(f.size() - 7);
        f.close();
        QVERIFY(!FolderIndex::load(file, QStringLiteral("key"), back));
        QVERIFY(!FolderIndex::load(file + QStringLiteral(".missing"), QStringLiteral("key"), back));
        QCOMPARE(back.size(), idx.size()); // untouched by the failed loads
    }

    // How long the real drive takes (GIFILES_BENCH_FOLDER_TREE=1): scan, cache, load, a search.
    void folderIndexBenchmark()
    {
        if (qEnvironmentVariableIsEmpty("GIFILES_BENCH_FOLDER_TREE"))
            QSKIP("set GIFILES_BENCH_FOLDER_TREE=1 to scan the whole drive");
        FolderIndex::Options o = FolderTree::options();
        QElapsedTimer t;
        t.start();
        const FolderIndex idx = FolderIndex::scan(o);
        const qint64 scanMs = t.restart();
        const QString file = m_root.filePath(QStringLiteral("bench.bin"));
        QVERIFY(idx.save(file, o.key(), 0));
        const qint64 saveMs = t.restart();
        FolderIndex back;
        QVERIFY(FolderIndex::load(file, o.key(), back));
        const qint64 loadMs = t.restart();
        QStringList each;
        for (const char *q : {"a", "e", "gif", "src", "sigif", "한", "zzzzqx"}) {
            t.restart();
            const int hits = idx.match(QString::fromUtf8(q), {}, 2000, false, idx.find(QDir::homePath())).total;
            each << QStringLiteral("%1: %2 ms (%3)").arg(QString::fromUtf8(q)).arg(t.elapsed()).arg(hits);
        }
        t.restart();
        FolderTreeModel model;
        model.setIndex(std::make_shared<FolderIndex>(back));
        const qint64 modelMs = t.elapsed();
        // An FSEvents update: the home folder and its direct subfolders read again, the rest copied.
        QStringList changed = {QDir::homePath()};
        for (const QString &d : QDir::home().entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            changed << QDir::home().filePath(d);
        t.restart();
        const FolderIndex updated = FolderIndex::update(idx, o, changed, {});
        const qint64 updateMs = t.elapsed();
        qInfo().noquote() << QStringLiteral("folders %1, scan %2 ms, save %3 ms (%4 KB), load %5 ms, tree rows %6 ms, update of %7 folders %8 ms; searches %9")
                                 .arg(idx.size()).arg(scanMs).arg(saveMs).arg(QFileInfo(file).size() / 1024).arg(loadMs).arg(modelMs)
                                 .arg(changed.size()).arg(updateMs).arg(each.join(QStringLiteral(", ")));
        QCOMPARE(updated.size(), FolderIndex::scan(o).size());
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Unit unit;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&unit, argc, argv);
}

#include "unit_core.moc"
