#pragma once

#include <QString>

// Debug log of what the user did and how it went, one file per day ("gifiles-YYYY-MM-DD.log"),
// for analysing reports later. Off unless started (main.cpp starts it on the development machine,
// in the repository's logs/ folder; the tests never do). Lines: "HH:mm:ss.zzz [category] text".
//
// Logged: keys (with modifiers; plain typed characters only counted, never their text), menu
// actions with the window's state before and after, navigation, file jobs and their errors,
// message boxes shown, config reloads and problems, Qt warnings.
namespace Log {

void start(const QString &dir, int keepDays = 7);
bool enabled();
void write(const char *category, const QString &text);
// The key that was pressed just now (within ~100 ms), e.g. "⌘↓", to tell keys from clicks.
QString recentKey();

} // namespace Log
