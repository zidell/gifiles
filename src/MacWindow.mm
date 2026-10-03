#include "MacWindow.h"

#include <QGuiApplication>
#include <QWidget>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

void macHideTitleText(QWidget *window)
{
    if (QGuiApplication::platformName() != QLatin1String("cocoa"))
        return; // e.g. the offscreen platform used by tests has no NSView behind winId()
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    if (NSWindow *w = view.window)
        w.titleVisibility = NSWindowTitleHidden;
}

void macDisableWindowAnimation(QWidget *window)
{
    if (QGuiApplication::platformName() != QLatin1String("cocoa"))
        return;
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    if (NSWindow *w = view.window)
        w.animationBehavior = NSWindowAnimationBehaviorNone;
}

long macWindowNumber(QWidget *window)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    return view.window ? long(view.window.windowNumber) : 0;
}

void macResetCursor()
{
    if (QGuiApplication::platformName() != QLatin1String("cocoa"))
        return;
    [[NSCursor arrowCursor] set];
}

static IMP s_qtHitTest = nullptr;

static id gifilesHitTest(id self, SEL cmd, NSPoint point)
{
    id hit = reinterpret_cast<id (*)(id, SEL, NSPoint)>(s_qtHitTest)(self, cmd, point);
    if (hit)
        return hit;
    NSView *view = self;
    return NSAccessibilityUnignoredAncestor(view) ?: view.window;
}

void macFixAccessibilityHitTest()
{
    Class cls = NSClassFromString(@"QNSView");
    if (!cls || s_qtHitTest)
        return;
    const SEL sel = @selector(accessibilityHitTest:);
    Method m = class_getInstanceMethod(cls, sel);
    if (!m || m == class_getInstanceMethod(class_getSuperclass(cls), sel))
        return; // Qt changed: QNSView no longer implements it, leave AppKit alone
    s_qtHitTest = method_setImplementation(m, reinterpret_cast<IMP>(gifilesHitTest));
}
