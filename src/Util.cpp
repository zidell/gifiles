#include "Util.h"

#include <cmath>

#include <QDir>
#include <QHash>
#include <QKeySequence>
#include <QLocale>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QUrl>

namespace util {

QString humanSize(qint64 n)
{
    if (n < 1000)
        return Gifiles::tr("%1바이트").arg(n);
    static const char *units[] = {"KB", "MB", "GB", "TB", "PB"};
    // Shown with one decimal below 10, none above; the unit steps up when the rounded value reaches
    // 1000, so 999,999 bytes is "1.0 MB", not "1000 KB".
    const auto rounded = [](double v) { return v < 9.95 ? std::round(v * 10) / 10 : std::round(v); };
    double f = n / 1000.0;
    int i = 0;
    while (rounded(f) >= 1000 && i < 4) {
        f /= 1000;
        ++i;
    }
    const double r = rounded(f);
    return QStringLiteral("%1 %2").arg(QLocale().toString(r, 'f', r < 10 ? 1 : 0), units[i]);
}

QString humanDate(const QDateTime &t)
{
    if (!t.isValid())
        return QStringLiteral("--");
    return QStringLiteral("%1. %2. %3. %4").arg(t.date().year()).arg(t.date().month()).arg(t.date().day())
                                             .arg(t.toString(QStringLiteral("HH:mm")));
}

namespace {
enum Cat { File, Dir, Image, Text, Code, Audio, Video, Archive, Pdf, Exec, App };

const QHash<QString, Cat> &extMap()
{
    static const QHash<QString, Cat> m = [] {
        QHash<QString, Cat> h;
        auto reg = [&](Cat c, std::initializer_list<const char *> exts) {
            for (auto e : exts)
                h.insert(QString::fromLatin1(e), c);
        };
        reg(Image, {"png", "jpg", "jpeg", "gif", "bmp", "webp", "tif", "tiff", "heic", "heif", "svg", "ico", "icns", "avif"});
        reg(Text, {"txt", "md", "markdown", "rst", "log", "csv", "tsv", "rtf"});
        reg(Code, {"go", "py", "js", "mjs", "ts", "tsx", "jsx", "c", "h", "cpp", "hpp", "cc", "rs", "java", "kt",
                   "swift", "rb", "php", "sh", "zsh", "bash", "fish", "lua", "pl", "sql", "html", "htm", "css",
                   "scss", "json", "yaml", "yml", "toml", "xml", "ini", "conf", "cfg", "kdl", "vue", "svelte",
                   "dart", "ex", "exs", "cmake", "gradle", "m", "mm", "cs", "ps1"});
        reg(Audio, {"mp3", "wav", "flac", "aac", "m4a", "ogg", "opus", "aiff", "aif"});
        reg(Video, {"mp4", "mov", "mkv", "avi", "webm", "m4v", "wmv", "mpg", "mpeg"});
        reg(Archive, {"zip", "tar", "gz", "tgz", "bz2", "xz", "7z", "rar", "zst", "dmg", "pkg", "iso"});
        reg(Pdf, {"pdf"});
        reg(Exec, {"exe", "bat", "cmd", "msi", "com"});
        return h;
    }();
    return m;
}

Cat categoryOf(const QFileInfo &fi)
{
    if (fi.isDir())
        return isPackage(fi) ? App : Dir;
    auto it = extMap().constFind(fi.suffix().toLower());
    if (it != extMap().constEnd())
        return *it;
    if (fi.isExecutable())
        return Exec;
    return File;
}
} // namespace

QString kindOf(const QFileInfo &fi)
{
    const QString ext = fi.suffix().toUpper();
    switch (categoryOf(fi)) {
    case Dir: return Gifiles::tr("폴더");
    case App: return Gifiles::tr("응용 프로그램");
    case Image: return ext + Gifiles::tr(" 이미지");
    case Text: return Gifiles::tr("텍스트");
    case Code: return ext + Gifiles::tr(" 소스");
    case Audio: return ext + Gifiles::tr(" 오디오");
    case Video: return ext + Gifiles::tr(" 동영상");
    case Archive: return ext + Gifiles::tr(" 압축 파일");
    case Pdf: return Gifiles::tr("PDF 문서");
    case Exec: return Gifiles::tr("실행 파일");
    case File: break;
    }
    return ext.isEmpty() ? Gifiles::tr("문서") : ext + Gifiles::tr(" 파일");
}

void splitExt(const QString &name, QString *base, QString *ext)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        *base = name;
        ext->clear();
        return;
    }
    *base = name.left(dot);
    *ext = name.mid(dot);
}

bool exists(const QString &path)
{
    QFileInfo fi(path);
    return fi.exists() || fi.isSymLink();
}

QString uniqueCopyName(const QString &dir, const QString &name, bool forceCopy)
{
    if (!forceCopy && !exists(QDir(dir).filePath(name)))
        return name;
    QString base, ext;
    splitExt(name, &base, &ext);
    // "a 복사본 2" → "a", so repeated copies don't nest.
    static const QRegularExpression copySuffix(Gifiles::tr(" 복사본( \\d+)?$"));
    base.remove(copySuffix);
    for (int i = 1;; ++i) {
        QString cand = i == 1 ? base + Gifiles::tr(" 복사본") + ext
                              : Gifiles::tr("%1 복사본 %2%3").arg(base).arg(i).arg(ext);
        if (!exists(QDir(dir).filePath(cand)))
            return cand;
    }
}

QString uniquePlainName(const QString &dir, const QString &name)
{
    if (!exists(QDir(dir).filePath(name)))
        return name;
    QString base, ext;
    splitExt(name, &base, &ext);
    for (int i = 2;; ++i) {
        QString cand = QStringLiteral("%1 %2%3").arg(base).arg(i).arg(ext);
        if (!exists(QDir(dir).filePath(cand)))
            return cand;
    }
}

bool isInside(const QString &child, const QString &parent)
{
    const QString c = QDir::cleanPath(child), p = QDir::cleanPath(parent);
    if (c == p)
        return true;
    return c.startsWith(p.endsWith(QLatin1Char('/')) ? p : p + QLatin1Char('/'));
}

int naturalCompare(QStringView a, QStringView b)
{
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].isDigit() && b[j].isDigit()) {
            qsizetype ie = i, je = j;
            while (ie < a.size() && a[ie].isDigit())
                ++ie;
            while (je < b.size() && b[je].isDigit())
                ++je;
            QStringView na = a.sliced(i, ie - i), nb = b.sliced(j, je - j);
            while (na.size() > 1 && na.front() == u'0')
                na = na.sliced(1);
            while (nb.size() > 1 && nb.front() == u'0')
                nb = nb.sliced(1);
            if (na.size() != nb.size())
                return na.size() < nb.size() ? -1 : 1;
            if (const int c = na.compare(nb))
                return c;
            i = ie;
            j = je;
            continue;
        }
        const QChar ca = a[i].toCaseFolded(), cb = b[j].toCaseFolded();
        if (ca != cb)
            return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size() || j < b.size())
        return i < a.size() ? 1 : -1;
    return a.compare(b);
}

bool isPackage(const QFileInfo &fi)
{
#ifdef Q_OS_MACOS
    return fi.isBundle();
#else
    Q_UNUSED(fi);
    return false;
#endif
}

bool isHexColor(const QString &s)
{
    static const QRegularExpression re(QRegularExpression::anchoredPattern(QStringLiteral("#[0-9A-Fa-f]{6}"))); // "$" would let "\n" through
    return re.match(s).hasMatch();
}

QStringList extensionList(const QString &text)
{
    QStringList out;
    for (QString e : text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)) {
        while (e.startsWith(QLatin1Char('.')))
            e.remove(0, 1);
        if (!e.isEmpty() && !out.contains(e.toLower()))
            out << e.toLower();
    }
    return out;
}

QString validateName(const QString &name)
{
    if (name.trimmed().isEmpty())
        return Gifiles::tr("이름이 비어 있습니다.");
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return Gifiles::tr("사용할 수 없는 이름입니다.");
#ifdef Q_OS_WIN
    static const QString bad = QStringLiteral("\\/:*?\"<>|");
#else
    static const QString bad = QStringLiteral("/");
#endif
    for (QChar c : bad)
        if (name.contains(c))
            return Gifiles::tr("이름에 \"%1\" 문자를 쓸 수 없습니다.").arg(bad);
    return {};
}

QString displayName(const QString &path)
{
    QFileInfo fi(path);
    QString n = fi.fileName();
    if (!n.isEmpty())
        return n;
    QStorageInfo si(path);
    if (si.isValid() && !si.displayName().isEmpty() && si.rootPath() == QDir::cleanPath(path))
        return si.displayName();
    return QDir::toNativeSeparators(path);
}

QString nativeShortcutText(const QString &portable)
{
    return QKeySequence(portable).toString(QKeySequence::NativeText);
}

Command revealCommand(const QString &path)
{
    const QFileInfo fi(path);
    const QString abs = fi.absoluteFilePath();
#if defined(Q_OS_MACOS)
    return fi.isDir() ? Command{QStringLiteral("open"), {abs}} : Command{QStringLiteral("open"), {QStringLiteral("-R"), abs}};
#elif defined(Q_OS_WIN)
    const QString target = QDir::toNativeSeparators(abs);
    return {QStringLiteral("explorer.exe"), {fi.isDir() ? target : QStringLiteral("/select,") + target}};
#else
    // dbus-send splits array items at commas.
    const QString url = QUrl::fromLocalFile(abs).toString(QUrl::FullyEncoded).replace(QLatin1Char(','), QLatin1String("%2C"));
    return {QStringLiteral("dbus-send"),
            {QStringLiteral("--session"), QStringLiteral("--print-reply"), QStringLiteral("--dest=org.freedesktop.FileManager1"),
             QStringLiteral("--type=method_call"), QStringLiteral("/org/freedesktop/FileManager1"),
             fi.isDir() ? QStringLiteral("org.freedesktop.FileManager1.ShowFolders") : QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
             QStringLiteral("array:string:") + url, QStringLiteral("string:")}};
#endif
}

QString revealActionId()
{
#if defined(Q_OS_MACOS)
    return QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "Finder에서 열기"));
#elif defined(Q_OS_WIN)
    return QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "탐색기에서 열기"));
#else
    return QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "파일 관리자에서 열기"));
#endif
}

QString revealActionText()
{
    return Gifiles::tr(revealActionId().toUtf8().constData());
}

QString revealShowText()
{
#if defined(Q_OS_MACOS)
    return Gifiles::tr("Finder에서 보기");
#elif defined(Q_OS_WIN)
    return Gifiles::tr("탐색기에서 보기");
#else
    return Gifiles::tr("파일 관리자에서 보기");
#endif
}

} // namespace util
