#include "Toml.h"
#include "Util.h"

namespace toml {
namespace {

struct Reader {
    const QString &s;
    qsizetype i = 0;
    int line = 1;

    bool atEnd() const { return i >= s.size(); }
    QChar peek() const { return atEnd() ? QChar() : s[i]; }
    void skipSpace() // spaces and tabs only
    {
        while (!atEnd() && (s[i] == QLatin1Char(' ') || s[i] == QLatin1Char('\t')))
            ++i;
    }
    void skipComment()
    {
        if (peek() == QLatin1Char('#'))
            while (!atEnd() && s[i] != QLatin1Char('\n'))
                ++i;
    }
    // Spaces, comments and newlines (inside arrays).
    void skipAll()
    {
        for (;;) {
            skipSpace();
            skipComment();
            if (peek() == QLatin1Char('\r'))
                ++i;
            if (peek() == QLatin1Char('\n')) {
                ++i;
                ++line;
                continue;
            }
            return;
        }
    }
    bool endOfLine(QString &err)
    {
        skipSpace();
        skipComment();
        if (peek() == QLatin1Char('\r'))
            ++i;
        if (atEnd())
            return true;
        if (peek() != QLatin1Char('\n')) {
            err = Gifiles::tr("값 뒤에 알 수 없는 내용이 있습니다");
            return false;
        }
        return true;
    }

    bool readString(QString &out, QString &err)
    {
        ++i; // opening quote
        out.clear();
        while (!atEnd()) {
            const QChar c = s[i++];
            if (c == QLatin1Char('"'))
                return true;
            if (c == QLatin1Char('\n'))
                break;
            if (c != QLatin1Char('\\')) {
                out += c;
                continue;
            }
            if (atEnd())
                break;
            const QChar e = s[i++];
            switch (e.unicode()) {
            case '"': out += QLatin1Char('"'); break;
            case '\\': out += QLatin1Char('\\'); break;
            case 'n': out += QLatin1Char('\n'); break;
            case 't': out += QLatin1Char('\t'); break;
            case 'r': out += QLatin1Char('\r'); break;
            case 'u': {
                bool ok = false;
                const uint code = s.mid(i, 4).toUInt(&ok, 16);
                if (!ok) {
                    err = Gifiles::tr("잘못된 \\u 이스케이프");
                    return false;
                }
                out += QChar(char16_t(code));
                i += 4;
                break;
            }
            default:
                err = Gifiles::tr("지원하지 않는 이스케이프 \\%1").arg(e);
                return false;
            }
        }
        err = Gifiles::tr("문자열이 닫히지 않았습니다 (\")");
        return false;
    }

    bool readKey(QString &out, QString &err)
    {
        if (peek() == QLatin1Char('"'))
            return readString(out, err);
        const qsizetype start = i;
        while (!atEnd() && (s[i].isLetterOrNumber() || s[i] == QLatin1Char('_') || s[i] == QLatin1Char('-')))
            ++i;
        out = s.mid(start, i - start);
        if (out.isEmpty()) {
            err = Gifiles::tr("키가 없습니다");
            return false;
        }
        return true;
    }

    // { key = "string", key = "string" } on one line.
    bool readInlineTable(QVariantMap &out, QString &err)
    {
        ++i; // {
        for (;;) {
            skipSpace();
            if (peek() == QLatin1Char('}')) {
                ++i;
                return true;
            }
            QString k, v;
            if (!readKey(k, err))
                return false;
            skipSpace();
            if (peek() != QLatin1Char('=')) {
                err = Gifiles::tr("'%1' 뒤에 = 가 필요합니다").arg(k);
                return false;
            }
            ++i;
            skipSpace();
            if (s.mid(i, 4) == QLatin1String("true") || s.mid(i, 5) == QLatin1String("false")) {
                const bool on = s.mid(i, 4) == QLatin1String("true");
                i += on ? 4 : 5;
                out.insert(k, on);
            } else if (peek() != QLatin1Char('"')) {
                err = Gifiles::tr("'%1'의 값은 \"문자열\"이나 true / false여야 합니다").arg(k);
                return false;
            } else {
                if (!readString(v, err))
                    return false;
                out.insert(k, v);
            }
            skipSpace();
            if (peek() == QLatin1Char(','))
                ++i;
            else if (peek() != QLatin1Char('}')) {
                err = Gifiles::tr("{ } 안의 항목 사이에 쉼표(,)가 필요합니다");
                return false;
            }
        }
    }

    bool readValue(QVariant &out, QString &err)
    {
        const QChar c = peek();
        if (c == QLatin1Char('"')) {
            QString str;
            if (!readString(str, err))
                return false;
            out = str;
            return true;
        }
        if (c == QLatin1Char('[')) {
            ++i;
            QStringList list;
            QVariantList tables; // an array of inline tables: [ { a = "x" }, ... ]
            for (;;) {
                skipAll();
                if (peek() == QLatin1Char(']')) {
                    ++i;
                    if (!tables.isEmpty())
                        out = tables;
                    else
                        out = list;
                    return true;
                }
                if (peek() == QLatin1Char('{') && list.isEmpty()) {
                    QVariantMap table;
                    if (!readInlineTable(table, err))
                        return false;
                    tables << table;
                } else if (peek() == QLatin1Char('"') && tables.isEmpty()) {
                    QString str;
                    if (!readString(str, err))
                        return false;
                    list << str;
                } else {
                    err = Gifiles::tr("배열에는 \"문자열\"이나 { 키 = \"문자열\" } 중 한 가지만 쓸 수 있습니다");
                    return false;
                }
                skipAll();
                if (peek() == QLatin1Char(','))
                    ++i;
                else if (peek() != QLatin1Char(']')) {
                    err = Gifiles::tr("배열 항목 사이에 쉼표(,)가 필요합니다");
                    return false;
                }
            }
        }
        const qsizetype start = i;
        while (!atEnd() && !s[i].isSpace() && s[i] != QLatin1Char('#'))
            ++i;
        const QString word = s.mid(start, i - start);
        if (word == QLatin1String("true") || word == QLatin1String("false")) {
            out = word == QLatin1String("true");
            return true;
        }
        bool ok = false;
        const qlonglong n = QString(word).remove(QLatin1Char('_')).toLongLong(&ok);
        if (ok) {
            out = n;
            return true;
        }
        err = word.isEmpty() ? Gifiles::tr("값이 없습니다")
                             : Gifiles::tr("알 수 없는 값 '%1' (문자열은 \"...\"로 감쌉니다)").arg(word);
        return false;
    }
};

} // namespace

bool parse(const QString &text, Document &out, Error &error)
{
    Reader r{text};
    QString table;
    out[table];
    QString err;
    while (!r.atEnd()) {
        r.skipSpace();
        r.skipComment();
        if (r.peek() == QLatin1Char('\r'))
            ++r.i;
        if (r.peek() == QLatin1Char('\n')) {
            ++r.i;
            ++r.line;
            continue;
        }
        if (r.atEnd())
            break;
        const int line = r.line;
        if (r.peek() == QLatin1Char('[')) {
            ++r.i;
            r.skipSpace();
            if (!r.readKey(table, err)) {
                error = {line, err};
                return false;
            }
            r.skipSpace();
            if (r.peek() != QLatin1Char(']')) {
                error = {line, Gifiles::tr("표 이름 뒤에 ]가 필요합니다")};
                return false;
            }
            ++r.i;
            out[table];
        } else {
            QString k;
            QVariant v;
            if (!r.readKey(k, err)) {
                error = {line, err};
                return false;
            }
            r.skipSpace();
            if (r.peek() != QLatin1Char('=')) {
                error = {line, Gifiles::tr("'%1' 뒤에 = 가 필요합니다").arg(k)};
                return false;
            }
            ++r.i;
            r.skipSpace();
            if (!r.readValue(v, err)) {
                error = {r.line, QStringLiteral("'%1': %2").arg(k, err)};
                return false;
            }
            if (out[table].contains(k)) {
                error = {line, Gifiles::tr("'%1'이(가) 두 번 나옵니다").arg(k)};
                return false;
            }
            out[table].insert(k, v);
        }
        if (!r.endOfLine(err)) {
            error = {r.line, err};
            return false;
        }
    }
    return true;
}

QString quote(const QString &s)
{
    QString o = QStringLiteral("\"");
    for (const QChar c : s) {
        switch (c.unicode()) {
        case '"': o += QStringLiteral("\\\""); break;
        case '\\': o += QStringLiteral("\\\\"); break;
        case '\n': o += QStringLiteral("\\n"); break;
        case '\t': o += QStringLiteral("\\t"); break;
        case '\r': o += QStringLiteral("\\r"); break;
        default:
            if (c.unicode() < 0x20 || c.unicode() == 0x7f)
                o += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
            else
                o += c;
        }
    }
    return o + QLatin1Char('"');
}

QString key(const QString &k)
{
    bool bare = !k.isEmpty();
    for (const QChar c : k)
        bare = bare && c.unicode() < 128 && (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-'));
    return bare ? k : quote(k);
}

QString value(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Bool: return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong: return QString::number(v.toLongLong());
    case QMetaType::QVariantList: { // inline tables, one per line
        QStringList rows;
        for (const QVariant &row : v.toList()) {
            QStringList kv;
            const QVariantMap m = row.toMap();
            QStringList keys = m.keys();
            // id, label, key, terminal (extensions) first and the (long) command last; anything else between.
            QStringList ordered;
            for (const char *first : {"id", "label", "key", "terminal", "extensions"})
                if (keys.removeOne(QLatin1String(first)))
                    ordered << QLatin1String(first);
            const bool hasCommand = keys.removeOne(QStringLiteral("command"));
            ordered << keys;
            if (hasCommand)
                ordered << QStringLiteral("command");
            for (const QString &k : ordered)
                kv << key(k) + QStringLiteral(" = ") + value(m.value(k));
            rows << QStringLiteral("  { ") + kv.join(QStringLiteral(", ")) + QStringLiteral(" },");
        }
        return rows.isEmpty() ? QStringLiteral("[]") : QStringLiteral("[\n") + rows.join(QLatin1Char('\n')) + QStringLiteral("\n]");
    }
    case QMetaType::QStringList: {
        QStringList parts;
        for (const QString &s : v.toStringList())
            parts << quote(s);
        return QLatin1Char('[') + parts.join(QStringLiteral(", ")) + QLatin1Char(']');
    }
    default: return quote(v.toString());
    }
}

} // namespace toml
