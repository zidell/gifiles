#include "SystemPreview.h"

#include <QFileInfo>
#include <QVBoxLayout>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <Quartz/Quartz.h>

namespace {

// Finder's own Quick Look view, inside a Qt window container.
class MacSystemPreview : public SystemPreview {
public:
    explicit MacSystemPreview(QWidget *parent) : SystemPreview(parent)
    {
        m_view = [[QLPreviewView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300) style:QLPreviewViewStyleNormal];
        m_view.shouldCloseWithWindow = NO; // the widget decides when it goes away
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        QWidget *container = QWidget::createWindowContainer(QWindow::fromWinId(WId(m_view)), this);
        container->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(container);
        // A click puts the keyboard in the Quick Look view, where Qt no longer sees Space, Esc or
        // the arrows; give it back to the Qt window before such a key is delivered.
        NSView *view = m_view;
        m_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                          handler:^NSEvent *(NSEvent *event) {
                                                              NSWindow *w = view.window;
                                                              NSResponder *r = w.firstResponder;
                                                              if (event.window == w && [r isKindOfClass:[NSView class]] &&
                                                                  [static_cast<NSView *>(r) isDescendantOf:view])
                                                                  [w makeFirstResponder:view.superview];
                                                              return event;
                                                          }];
        [m_monitor retain];
    }

    ~MacSystemPreview() override
    {
        [NSEvent removeMonitor:m_monitor];
        [m_monitor release];
        m_view.previewItem = nil;
        [m_view close];
        [m_view release];
    }

    bool show(const QString &path) override
    {
        m_view.previewItem = [NSURL fileURLWithPath:path.toNSString()];
        return true;
    }

    void clear() override { m_view.previewItem = nil; }

private:
    QLPreviewView *m_view;
    id m_monitor;
};

} // namespace

bool SystemPreview::canShow(const QString &path)
{
    const QFileInfo fi(path);
    return available() && fi.exists() && (!fi.isDir() || isOfficeDocument(path)); // Pages & co. can be packages
}

SystemPreview *SystemPreview::create(QWidget *parent)
{
    return available() ? new MacSystemPreview(parent) : nullptr;
}
