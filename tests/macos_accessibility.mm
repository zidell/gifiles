// Qt Cocoa placeholders borrow the table ID; destroying them must not delete it.
// Load the Cocoa provider on the offscreen platform: no desktop windows.
#include "Headless.h"
#include "MacWindow.h"

#include <QApplication>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QLibrary>
#include <QLibraryInfo>
#include <QWidget>
#include <QTreeWidget>
#include <cstdio>

#import <AppKit/AppKit.h>
#import <objc/message.h>

static int observedUpdates = 0;
static void observeUpdate(QAccessibleEvent *) {++observedUpdates;}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QLibrary cocoa(QLibraryInfo::path(QLibraryInfo::PluginsPath) + "/platforms/libqcocoa.dylib");
    cocoa.setLoadHints(QLibrary::PreventUnloadHint);
    if (!cocoa.load())
        return 2;
    macFixAccessibilityOwnership();
    QWidget widget;
    auto *iface = new QAccessibleWidget(&widget);
    const auto aid = QAccessible::registerAccessibleInterface(iface);
    Class cls = NSClassFromString(@"QMacAccessibilityElement");
    if (!cls)
        return 3;
    for (NSString *role in @[NSAccessibilityRowRole, NSAccessibilityColumnRole, NSAccessibilityCellRole]) {
        id placeholder = reinterpret_cast<id (*)(id, SEL, unsigned, id)>(objc_msgSend)(
            [cls alloc], sel_registerName("initWithId:role:"), aid, role);
        // Clearing cached arrays and deallocating placeholders are separate paths.
        reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(
            cls, sel_registerName("removeElementsFromCache:"), @[placeholder]);
        if (QAccessible::accessibleInterface(aid) != iface)
            return 4;
        [placeholder release];
        if (QAccessible::accessibleInterface(aid) != iface)
            return 1;
    }
    // Cocoa must not coalesce different borrowed-ID rows/columns/cells into one AX element.
    for (NSString *role in @[NSAccessibilityRowRole, NSAccessibilityColumnRole, NSAccessibilityCellRole]) {
        id a = reinterpret_cast<id (*)(id, SEL, unsigned, id)>(objc_msgSend)([cls alloc], sel_registerName("initWithId:role:"), aid, role);
        id b = reinterpret_cast<id (*)(id, SEL, unsigned, id)>(objc_msgSend)([cls alloc], sel_registerName("initWithId:role:"), aid, role);
        if ([a isEqual:b] || [b isEqual:a] || ![a isEqual:a] || [NSSet setWithObjects:a,b,nil].count!=2)
            return 10;
        [a release];[b release];
    }
    // Real elements must still delete their own interfaces normally.
    id real = reinterpret_cast<id (*)(id, SEL, unsigned, id)>(objc_msgSend)(
        [cls alloc], sel_registerName("initWithId:role:"), aid, nil);
    [real release];
    if (QAccessible::accessibleInterface(aid))
        return 5;
    QTreeWidget tree;
    tree.setHeaderHidden(true);tree.setRootIsDecorated(false);tree.setIndentation(0);
    auto fill=[&] {tree.clear();auto *section=new QTreeWidgetItem(&tree,{"Section"});new QTreeWidgetItem(section,{"First"});new QTreeWidgetItem(section,{"Second"});section->setExpanded(true);};
    fill();tree.resize(240,300);tree.show();app.processEvents();
    auto *ti=QAccessible::queryAccessibleInterface(&tree);
    auto native=reinterpret_cast<id (*)(id,SEL,QAccessibleInterface*)>(objc_msgSend)(cls,sel_registerName("elementWithInterface:"),ti);
    [native retain];
    for(int pass=0;pass<3;++pass){
        if(pass){
            // Cocoa rebuilds rows on DataChanged while Qt keeps its child cache.
            auto *before=ti->tableInterface()->cellAt(0,0);
            reinterpret_cast<void (*)(id,SEL)>(objc_msgSend)(native,sel_registerName("updateTableModel"));
            auto *cell=ti->tableInterface()->cellAt(0,0);
            if(cell!=before)return 12; // DataChanged preserves valid child identities.
            if(!cell||!cell->isValid()||cell->rect().isEmpty())return 11;
            fill();app.processEvents();
        }
        auto previousHandler=QAccessible::installUpdateHandler(observeUpdate);
        QAccessible::setActive(true);
        observedUpdates=0;
        NSArray *rs=reinterpret_cast<id (*)(id,SEL)>(objc_msgSend)(native,sel_registerName("accessibilityRows"));
        if(observedUpdates!=0)return 13;
        QAccessibleEvent probe(&tree,QAccessible::NameChanged);
        QAccessible::updateAccessibility(&probe);
        if(observedUpdates!=1)return 14;
        QAccessible::setActive(false);
        QAccessible::installUpdateHandler(previousHandler);
        if(rs.count!=3)return 8;
        for(id row in rs){
            NSRect r=reinterpret_cast<NSRect (*)(id,SEL)>(objc_msgSend)(row,sel_registerName("accessibilityFrame"));
            NSArray *cs=reinterpret_cast<id (*)(id,SEL)>(objc_msgSend)(row,sel_registerName("accessibilityChildren"));
            if(cs.count!=1)return 9;
            for(id cell in cs){
                NSRect cr=reinterpret_cast<NSRect (*)(id,SEL)>(objc_msgSend)(cell,sel_registerName("accessibilityFrame"));
                std::fprintf(stderr,"pass=%d cell=%g,%g,%g,%g\n",pass,cr.origin.x,cr.origin.y,cr.size.width,cr.size.height);
                if(NSIsEmptyRect(cr))return 6;
            }
            std::fprintf(stderr,"pass=%d row=%g,%g,%g,%g\n",pass,r.origin.x,r.origin.y,r.size.width,r.size.height);
            if(NSIsEmptyRect(r))return 7;
        }
    }
    [native release];
    return 0;
}
