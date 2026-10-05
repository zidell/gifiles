#pragma once

// Every test binary includes this first: it runs on Qt's offscreen platform unless
// QT_QPA_PLATFORM says otherwise, so no test window ever reaches the screen, shows in the Dock or
// takes the keyboard from the app the user is working in — whoever starts the tests, however.
// (Set QT_QPA_PLATFORM=cocoa / windows / xcb explicitly to watch a run on the real desktop.)

#include <QByteArray>
#include <QtGlobal>

namespace gifiles_test {
inline const bool headless = [] {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    return true;
}();
} // namespace gifiles_test
