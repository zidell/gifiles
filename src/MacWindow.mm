#include "MacWindow.h"

#include <QGuiApplication>
#include <QAccessible>
#include <QWidget>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cstring>

namespace {
IMP qtElementDealloc = nullptr;
IMP qtRemoveElements = nullptr;
IMP qtAccessibilityRows = nullptr;
IMP qtUpdateTableModel = nullptr;
IMP qtElementEqual = nullptr;
IMP qtElementHash = nullptr;
Ivar qtRows = nullptr;
Ivar qtColumns = nullptr;
Ivar qtSyntheticRole = nullptr;
ptrdiff_t qtAxIdOffset = 0;

bool isSyntheticElement(id element)
{
    return object_getIvar(element, qtSyntheticRole) != nil;
}

BOOL distinctElementEqual(id element, SEL cmd, id other)
{
    if (element == other)
        return YES;
    if (isSyntheticElement(element) || ([other isKindOfClass:object_getClass(element)] && isSyntheticElement(other)))
        return NO;
    return reinterpret_cast<BOOL (*)(id, SEL, id)>(qtElementEqual)(element, cmd, other);
}

NSUInteger distinctElementHash(id element, SEL cmd)
{
    if (isSyntheticElement(element))
        return reinterpret_cast<NSUInteger>(element);
    return reinterpret_cast<NSUInteger (*)(id, SEL)>(qtElementHash)(element, cmd);
}

void resetTableCache(QAccessibleInterface *iface)
{
    // GUI-thread synchronous transaction: deleteInterface otherwise sends an
    // ObjectDestroyed event before removing the ID. Cocoa recreates a native cell
    // from that dying interface and can reenter the same cache while it is cleared.
    // Cache removal still calls native invalidate and posts its Destroyed notification.
    struct RestoreHandler {
        QAccessible::UpdateHandler previous;
        RestoreHandler():previous(QAccessible::installUpdateHandler([](QAccessibleEvent *) {})){}
        ~RestoreHandler(){QAccessible::installUpdateHandler(previous);}
        RestoreHandler(const RestoreHandler &) = delete;
        RestoreHandler &operator=(const RestoreHandler &) = delete;
    } restore;
    QAccessibleTableModelChangeEvent reset(iface, QAccessibleTableModelChangeEvent::ModelReset);
    iface->tableInterface()->modelChange(&reset);
}

struct NativeCellState {bool real = false;bool stale = false;};
NativeCellState nativeCellState(id element)
{
    NativeCellState state;
    NSArray *rows = object_getIvar(element, qtRows);
    NSUInteger rowIndex = 0;
    for (id row in rows) {
        NSArray *cells = object_getIvar(row, qtColumns);
        NSUInteger columnIndex = 0;
        for (id cell in cells) {
            if (!isSyntheticElement(cell)) {
                state.real = true;
                unsigned aid = 0;
                std::memcpy(&aid, reinterpret_cast<char *>(cell) + qtAxIdOffset, sizeof(aid));
                auto *iface = QAccessible::accessibleInterface(aid);
                auto *item = iface && iface->isValid() ? iface->tableCellInterface() : nullptr;
                state.stale |= !item || item->rowIndex() != int(rowIndex) || item->columnIndex() != int(columnIndex);
            }
            ++columnIndex;
        }
        ++rowIndex;
    }
    return state;
}
void notifyTableLayout(id element)
{
    NSAccessibilityPostNotificationWithUserInfo(element, NSAccessibilityLayoutChangedNotification,
                                                @{NSAccessibilityUIElementsKey: @[element]});
}
void synchronizedTableModel(id element, SEL cmd)
{
    bool rebuilding = false;
    if (!isSyntheticElement(element) && object_getIvar(element, qtRows)) {
        unsigned aid = 0;
        std::memcpy(&aid, reinterpret_cast<char *>(element) + qtAxIdOffset, sizeof(aid));
        auto *iface = QAccessible::accessibleInterface(aid);
        if (iface && iface->tableInterface()
            && [(NSArray *)object_getIvar(element, qtRows) count] == NSUInteger(iface->tableInterface()->rowCount())
            && [(NSArray *)object_getIvar(element, qtColumns) count] == NSUInteger(iface->tableInterface()->columnCount())) {
            auto state = nativeCellState(element);
            // DataChanged already reads fresh values through persistent indices.
            // Keep valid cell identities instead of destroying them on selection changes.
            if (state.real && !state.stale)
                return;
            resetTableCache(iface);
            rebuilding = true;
        }
    }
    // Shape-mismatch calls can come from a real cell's constructor. Never reset
    // its C++ interface, or notify/reenter the bridge, while that cell is being created.
    reinterpret_cast<void (*)(id, SEL)>(qtUpdateTableModel)(element, cmd);
    if (rebuilding)
        notifyTableLayout(element);
}
NSArray *freshAccessibilityRows(id element, SEL cmd)
{
    if (!isSyntheticElement(element)) {
        unsigned aid = 0;
        std::memcpy(&aid, reinterpret_cast<char *>(element) + qtAxIdOffset, sizeof(aid));
        auto *iface = QAccessible::accessibleInterface(aid);
        auto *table = iface ? iface->tableInterface() : nullptr;
        if (table) {
            bool shapeChanged = [(NSArray *)object_getIvar(element, qtRows) count] != NSUInteger(table->rowCount())
                || [(NSArray *)object_getIvar(element, qtColumns) count] != NSUInteger(table->columnCount());
            auto state = nativeCellState(element);
            if (shapeChanged || state.stale || !state.real) {
                resetTableCache(iface);
                if (shapeChanged || state.stale) {
                    reinterpret_cast<void (*)(id, SEL)>(qtUpdateTableModel)(element, sel_registerName("updateTableModel"));
                    notifyTableLayout(element);
                }
            }
        }
    }
    return reinterpret_cast<NSArray *(*)(id, SEL)>(qtAccessibilityRows)(element, cmd);
}

void safeElementDealloc(id element, SEL cmd)
{
    // These placeholders borrow the table ID; only real cells own an ID.
    // Qt's dealloc otherwise deletes the table (and can reenter cell deletion).
    if (isSyntheticElement(element)) {
        const unsigned zero = 0;
        std::memcpy(reinterpret_cast<char *>(element) + qtAxIdOffset, &zero, sizeof(zero));
    }
    reinterpret_cast<void (*)(id, SEL)>(qtElementDealloc)(element, cmd);
}

void safeRemoveElements(id cls, SEL cmd, NSArray *elements)
{
    NSMutableArray *owned = [NSMutableArray arrayWithCapacity:elements.count];
    for (id element in elements) {
        if (!isSyntheticElement(element))
            [owned addObject:element];
    }
    reinterpret_cast<void (*)(id, SEL, NSArray *)>(qtRemoveElements)(cls, cmd, owned);
}
}

void macFixAccessibilityOwnership()
{
    // Private Cocoa implementation workaround: don't apply to other Qt versions.
    if (qstrcmp(qVersion(), "6.11.2") != 0 || qtElementDealloc)
        return;
    Class cls = NSClassFromString(@"QMacAccessibilityElement");
    if (!cls)
        return;
    Ivar axid = class_getInstanceVariable(cls, "axid");
    Ivar role = class_getInstanceVariable(cls, "synthesizedRole");
    Ivar rows = class_getInstanceVariable(cls, "rows");
    Ivar columns = class_getInstanceVariable(cls, "columns");
    Method accessibilityRows = class_getInstanceMethod(cls, sel_registerName("accessibilityRows"));
    Method updateTableModel = class_getInstanceMethod(cls, sel_registerName("updateTableModel"));
    Method equal = class_getInstanceMethod(cls, @selector(isEqual:));
    Method hash = class_getInstanceMethod(cls, @selector(hash));
    Method dealloc = class_getInstanceMethod(cls, sel_registerName("dealloc"));
    Method remove = class_getClassMethod(cls, sel_registerName("removeElementsFromCache:"));
    if (!axid || !role || !rows || !columns || !accessibilityRows || !updateTableModel || !equal || !hash || !dealloc || !remove || std::strcmp(ivar_getTypeEncoding(axid), "I") != 0)
        return;
    qtSyntheticRole = role;
    qtRows = rows;
    qtColumns = columns;
    qtAxIdOffset = ivar_getOffset(axid);
    qtUpdateTableModel = method_setImplementation(updateTableModel, reinterpret_cast<IMP>(synchronizedTableModel));
    qtElementEqual = method_setImplementation(equal, reinterpret_cast<IMP>(distinctElementEqual));
    qtElementHash = method_setImplementation(hash, reinterpret_cast<IMP>(distinctElementHash));
    qtAccessibilityRows = method_setImplementation(accessibilityRows, reinterpret_cast<IMP>(freshAccessibilityRows));
    qtElementDealloc = method_setImplementation(dealloc, reinterpret_cast<IMP>(safeElementDealloc));
    qtRemoveElements = method_setImplementation(remove, reinterpret_cast<IMP>(safeRemoveElements));
}

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

bool macLeftButtonDown()
{
    return ([NSEvent pressedMouseButtons] & 1) != 0;
}

void macResetCursor()
{
    if (QGuiApplication::platformName() != QLatin1String("cocoa"))
        return;
    [[NSCursor arrowCursor] set];
}

void macResetCursorOnDeactivate()
{
    static bool installed = false;
    if (installed || QGuiApplication::platformName() != QLatin1String("cocoa"))
        return;
    installed = true;
    // "Will" resign: the app is still active, so the window server still takes its cursor.
    [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationWillResignActiveNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification *) {
                                                      [[NSCursor arrowCursor] set];
                                                  }];
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

bool macIsPackage(const QString &path)
{
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:path.toNSString()];
        NSNumber *package = nil;
        return [url getResourceValue:&package forKey:NSURLIsPackageKey error:nil] && package.boolValue;
    }
}

void macStayInBackground()
{
    // After QApplication (Qt's Cocoa plugin makes the app a regular one). Accessory isn't enough:
    // showing and activating windows still brought the app to the front.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
}
