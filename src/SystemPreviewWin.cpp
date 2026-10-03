#include "SystemPreview.h"

#include <QDir>
#include <QFileInfo>
#include <QResizeEvent>

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

// The preview handler registered for the file's extension (its CLSID), as Explorer finds it.
bool handlerFor(const QString &path, CLSID *clsid)
{
    const QString ext = QLatin1Char('.') + QFileInfo(path).suffix();
    if (ext.size() < 2)
        return false;
    wchar_t buf[64];
    DWORD n = ARRAYSIZE(buf);
    // {8895b1c6-...}: IID_IPreviewHandler, the key preview handlers register under.
    if (FAILED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, reinterpret_cast<LPCWSTR>(ext.utf16()),
                                 L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", buf, &n)))
        return false;
    return SUCCEEDED(CLSIDFromString(buf, clsid));
}

// Hosts the handler in a native child window; the handler draws into it (most run in
// prevhost.exe, out of process).
class WinSystemPreview : public SystemPreview {
public:
    explicit WinSystemPreview(QWidget *parent) : SystemPreview(parent)
    {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_DontCreateNativeAncestors);
    }

    ~WinSystemPreview() override { clear(); }

    bool show(const QString &path) override
    {
        clear();
        CLSID clsid;
        if (!handlerFor(path, &clsid))
            return false;
        ComPtr<IPreviewHandler> h;
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&h))))
            return false;
        const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
        bool ready = false;
        ComPtr<IInitializeWithFile> withFile;
        ComPtr<IInitializeWithItem> withItem;
        ComPtr<IInitializeWithStream> withStream;
        if (SUCCEEDED(h.As(&withFile))) {
            ready = SUCCEEDED(withFile->Initialize(file.c_str(), STGM_READ));
        } else if (SUCCEEDED(h.As(&withItem))) {
            ComPtr<IShellItem> item;
            ready = SUCCEEDED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
                    SUCCEEDED(withItem->Initialize(item.Get(), STGM_READ));
        } else if (SUCCEEDED(h.As(&withStream))) {
            ComPtr<IStream> stream;
            ready = SUCCEEDED(SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream)) &&
                    SUCCEEDED(withStream->Initialize(stream.Get(), STGM_READ));
        }
        if (!ready)
            return false;
        const RECT r = nativeRect();
        if (FAILED(h->SetWindow(reinterpret_cast<HWND>(winId()), &r)) || FAILED(h->DoPreview())) {
            h->Unload();
            return false;
        }
        m_handler = h;
        return true;
    }

    void clear() override
    {
        if (m_handler) {
            m_handler->Unload();
            m_handler.Reset();
        }
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        SystemPreview::resizeEvent(e);
        if (m_handler) {
            const RECT r = nativeRect();
            m_handler->SetRect(&r);
        }
    }

private:
    RECT nativeRect() const
    {
        const qreal dpr = devicePixelRatioF();
        return RECT{0, 0, LONG(width() * dpr), LONG(height() * dpr)};
    }

    ComPtr<IPreviewHandler> m_handler;
};

} // namespace

bool SystemPreview::canShow(const QString &path)
{
    CLSID clsid;
    return available() && QFileInfo(path).isFile() && handlerFor(path, &clsid);
}

SystemPreview *SystemPreview::create(QWidget *parent)
{
    return available() ? new WinSystemPreview(parent) : nullptr;
}
