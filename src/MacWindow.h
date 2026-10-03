#pragma once

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

// macOS: show the arrow cursor now (a window closing under the pointer leaves its cursor up).
void macResetCursor();

// macOS: the window's number in the window server (screencapture -l), for snapshots.
long macWindowNumber(QWidget *window);
