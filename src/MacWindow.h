#pragma once

class QString;
class QWidget;

// macOS: hide the title text so the transparent title bar shows only the traffic lights
// (the window keeps its title for Mission Control and the Window menu).
void macHideTitleText(QWidget *window);

// macOS: no open/close animation (Quick Look should vanish at once, not fade out).
void macDisableWindowAnimation(QWidget *window);

// macOS: make accessibility hit tests always land in a window. Qt's hit test returns nil over item
// views (the file list), which AppKit reports as "not implemented"; window managers such as
// BetterTouchTool then can't tell which window is under the mouse and won't move/resize it.
void macFixAccessibilityHitTest();

// macOS: whether the left mouse button is down right now (also while the system moves a window, when Qt
// gets no mouse events).
bool macLeftButtonDown();

// macOS: show the arrow cursor now (a window closing under the pointer leaves its cursor up).
void macResetCursor();

// macOS: the window's number in the window server (screencapture -l), for snapshots.
long macWindowNumber(QWidget *window);

// macOS: whether Finder shows this folder as one item (an app, a bundle, a document package), by
// NSURLIsPackageKey. Not QFileInfo::isBundle(): that creates a CFBundle (reading the folder's
// Info.plist), and CFBundles made on the GUI thread race with the ones Qt's file-info thread makes
// for the same folders inside CoreFoundation's bundle cache (a crash in CFBundleGetIdentifier).
bool macIsPackage(const QString &path);

// macOS: run without taking the keyboard focus from the app in front (no Dock icon, can't be
// activated; windows still show and render, as inactive ones). For GIFILES_SNAPSHOT runs, which show real windows while the user works elsewhere.
void macStayInBackground();
