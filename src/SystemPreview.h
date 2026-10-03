#pragma once

#include <QWidget>

// The operating system's own preview of a file Qt can't draw (Word/Excel/PowerPoint, Pages,
// Numbers, Keynote, RTF, fonts, …), embedded in the preview: macOS Quick Look (QLPreviewView),
// Windows preview handlers (what Explorer's preview pane uses; Office or another app installs
// them). Linux has none: PreviewWidget converts office documents to PDF with LibreOffice there.
class SystemPreview : public QWidget {
public:
    // A real windowing system to embed into (not the offscreen platform the tests use).
    static bool available();
    // Office and other documents that should go to the system first, even before the text view.
    static bool isOfficeDocument(const QString &path);
    // Whether the system has a preview for this file. macOS: every file (Quick Look shows at
    // least the icon); Windows: a preview handler is registered for its extension.
    static bool canShow(const QString &path);
    // nullptr where there is none (Linux, offscreen).
    static SystemPreview *create(QWidget *parent);

    virtual bool show(const QString &path) = 0; // false: it can't, the caller shows something else
    virtual void clear() = 0;                   // lets go of the file

protected:
    using QWidget::QWidget;
};
