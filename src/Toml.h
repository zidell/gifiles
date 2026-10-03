#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

// The part of TOML that config.toml uses: [tables], bare or "quoted" keys, and values that are
// booleans, integers, "strings", arrays of strings, or arrays of one-line inline tables of strings
// and booleans ({ label = "…", terminal = true, command = "…" }); arrays may span lines. '#' starts a comment.
namespace toml {

using Table = QMap<QString, QVariant>;   // key -> bool / qlonglong / QString / QStringList / QVariantList of QVariantMap
using Document = QMap<QString, Table>;   // table name ("" for keys before any table) -> keys

struct Error {
    int line = 0;
    QString message;
};

// Returns false (and fills `error`) on a syntax error; `out` is then incomplete.
bool parse(const QString &text, Document &out, Error &error);

QString quote(const QString &s);              // "..." with escapes
QString key(const QString &k);                // bare if possible, else quoted
QString value(const QVariant &v);             // true, 12, "text", ["a", "b"]

} // namespace toml
