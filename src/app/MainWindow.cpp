#include "app/MainWindow.h"

#include "common/Registry.h"

#include <dwmapi.h>
#include <shobjidl_core.h>
#include <wincodec.h>
#include <windowsx.h>

#include <cstring>
#include <format>

#include <algorithm>
#include <atomic>

namespace dgmod::app {
namespace {

constexpr UINT kMsgNotify = WM_APP + 1;
constexpr UINT kMsgDevices = WM_APP + 2;
constexpr UINT_PTR kTimerTick = 1;
constexpr UINT_PTR kTimerAnim = 2;
constexpr UINT_PTR kTimerDevices = 3;
constexpr UINT_PTR kTimerSmoke = 4;
constexpr UINT kTickMs = 250;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Forwards endpoint changes to the window (debounced there).
class DeviceNotifier final : public IMMNotificationClient {
public:
    explicit DeviceNotifier(HWND hwnd) : hwnd_(hwnd) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Post(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Post(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Post(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return Post(); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return Post(); }

private:
    HRESULT Post() {
        ::PostMessageW(hwnd_, kMsgDevices, 0, 0);
        return S_OK;
    }
    HWND hwnd_;
    std::atomic<ULONG> refs_{1};
};

// Moves a window of this process to a virtual desktop other than the current one (documented
// IVirtualDesktopManager; desktop ids come from the Explorer registry state).
constexpr CLSID kClsidVirtualDesktopManager = {0xaa509086, 0x5ca9, 0x4c25, {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};

bool MoveToOtherVirtualDesktop(HWND hwnd) {
    auto k = dgmod::reg::Open(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
                               KEY_QUERY_VALUE);
    if (!k) return false;
    const auto ids = dgmod::reg::ReadRaw(k->Get(), L"VirtualDesktopIDs");
    const auto cur = dgmod::reg::ReadRaw(k->Get(), L"CurrentVirtualDesktop");
    if (!ids || ids->data.size() < 2 * sizeof(GUID)) return false;
    ComPtr<IVirtualDesktopManager> vdm;
    if (FAILED(::CoCreateInstance(kClsidVirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&vdm)))) return false;
    for (size_t off = 0; off + sizeof(GUID) <= ids->data.size(); off += sizeof(GUID)) {
        GUID g{};
        std::memcpy(&g, ids->data.data() + off, sizeof(g));
        if (cur && cur->data.size() >= sizeof(GUID) && std::memcmp(&g, cur->data.data(), sizeof(g)) == 0) continue;
        if (SUCCEEDED(vdm->MoveWindowToDesktop(hwnd, g))) return true;
    }
    return false;
}

int64_t QpcNow() {
    LARGE_INTEGER v;
    ::QueryPerformanceCounter(&v);
    return v.QuadPart;
}

}  // namespace

MainWindow::~MainWindow() {
    if (enumerator_ && notifier_) enumerator_->UnregisterEndpointNotificationCallback(notifier_.Get());
}

double MainWindow::Now() const { return double(QpcNow() - startQpc_) / double(qpcFreq_); }

Result<void> MainWindow::Create(int showCmd) {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    qpcFreq_ = f.QuadPart;
    startQpc_ = QpcNow();
    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = inst;
    wc.hIcon = ::LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON),
                                                 ::GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::CreateSolidBrush(RGB(0x1C, 0x1C, 0x1C));
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc)) return FailWin32(L"RegisterClassEx");

    theme_ = ui::Theme::Load();
    const float s = static_cast<float>(::GetDpiForSystem()) / 96.f;
    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = std::min(static_cast<int>(1280 * s), static_cast<int>(work.right - work.left) - 40);
    const int h = std::min(static_cast<int>(840 * s), static_cast<int>(work.bottom - work.top) - 40);
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + (work.bottom - work.top - h) / 2;

    hwnd_ = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, kClassName, L"dgmod", WS_OVERLAPPEDWINDOW, x, y,
                              w, h, nullptr, nullptr, inst, this);
    if (!hwnd_) return FailWin32(L"CreateWindowEx");
    dpi_ = ::GetDpiForWindow(hwnd_);

    ApplyTheme();
    MARGINS margins{-1, -1, -1, -1};
    ::DwmExtendFrameIntoClientArea(hwnd_, &margins);
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    // No Mica: an explicit backdrop type of "none" and caption/border colors matching the graphite background.
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    COLORREF caption = RGB(0x1C, 0x1C, 0x1C), border = RGB(0x33, 0x33, 0x33);
    ::DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    ::DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    if (auto r = renderer_.InitForWindow(hwnd_); !r) return r;

    model_.hwnd = hwnd_;
    model_.notify = [hwnd = hwnd_] { ::PostMessageW(hwnd, kMsgNotify, 0, 0); };

    if (SUCCEEDED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_)))) {
        notifier_.Attach(new DeviceNotifier(hwnd_));
        enumerator_->RegisterEndpointNotificationCallback(notifier_.Get());
    }

    if (smokeSeconds_ > 0) {
        otherDesktop_ = MoveToOtherVirtualDesktop(hwnd_);
        if (!otherDesktop_) ::SetWindowPos(hwnd_, nullptr, -30000, -30000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        showCmd = SW_SHOWNOACTIVATE;
        ::SetTimer(hwnd_, kTimerSmoke, 800, nullptr);
    }
    ::ShowWindow(hwnd_, showCmd);
    ::UpdateWindow(hwnd_);
    ::SetTimer(hwnd_, kTimerTick, kTickMs, nullptr);
    return {};
}

int MainWindow::Run() {
    // Frames are paced by the DXGI frame-latency waitable object; when nothing changes the loop sleeps.
    for (;;) {
        const bool wantFrame = dirty_ || animating_;
        HANDLE waitable = renderer_.FrameWaitable();
        DWORD r;
        if (wantFrame && waitable)
            r = ::MsgWaitForMultipleObjectsEx(1, &waitable, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        else if (wantFrame)
            r = WAIT_OBJECT_0;
        else
            r = ::MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return static_cast<int>(msg.wParam);
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (wantFrame && r == WAIT_OBJECT_0 && (dirty_ || animating_)) Render();
    }
}

void MainWindow::ApplyTheme() {
    theme_ = ui::Theme::Load();
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    Invalidate();
}

void MainWindow::Render() {
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0 || !renderer_.Ready()) return;
    auto* dc = renderer_.BeginDraw(static_cast<float>(dpi_));
    if (!dc) {
        ::SetTimer(hwnd_, kTimerAnim, 100, nullptr);  // retry device recreation
        return;
    }
    model_.now = Now();
    const ui::Rect client{0, 0, static_cast<float>(rc.right) / Scale(), static_cast<float>(rc.bottom) / Scale()};
    ui_.Begin(dc, theme_, input_, model_.now, hwnd_);
    shell_.Draw(ui_, client, model_, shellState_);
    ui_.End();
    renderer_.EndDraw();
    ++frames_;
    input_.EndFrame();
    dirty_ = false;
    animating_ = ui_.WantsFrame();
    if (!inSizeMove_) ::KillTimer(hwnd_, kTimerAnim);
}

void MainWindow::SetMouse(LPARAM lp, bool screen) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (screen) ::ScreenToClient(hwnd_, &pt);
    input_.mouse = {static_cast<float>(pt.x) / Scale(), static_cast<float>(pt.y) / Scale()};
    input_.ctrl = ::GetKeyState(VK_CONTROL) < 0;
    input_.shift = ::GetKeyState(VK_SHIFT) < 0;
}

LRESULT MainWindow::HitTest(LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ::ScreenToClient(hwnd_, &pt);
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const int border = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi_) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
    if (!::IsZoomed(hwnd_) && pt.y < border) {
        if (pt.x < border * 2) return HTTOPLEFT;
        if (pt.x >= rc.right - border * 2) return HTTOPRIGHT;
        return HTTOP;
    }
    const float x = static_cast<float>(pt.x) / Scale();
    const float y = static_cast<float>(pt.y) / Scale();
    const float w = static_cast<float>(rc.right) / Scale();
    switch (Shell::CaptionHit(x, y, w)) {
        case CaptionButton::Minimize: return HTMINBUTTON;
        case CaptionButton::Maximize: return HTMAXBUTTON;
        case CaptionButton::Close: return HTCLOSE;
        default: break;
    }
    if (Shell::InDragRegion(x, y, w)) return HTCAPTION;
    return HTCLIENT;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->Handle(msg, wp, lp) : ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    auto nc = [](WPARAM hit) {
        switch (hit) {
            case HTMINBUTTON: return CaptionButton::Minimize;
            case HTMAXBUTTON: return CaptionButton::Maximize;
            case HTCLOSE: return CaptionButton::Close;
            default: return CaptionButton::None;
        }
    };
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const LONG top = p->rgrc[0].top;
                const LRESULT r = ::DefWindowProcW(hwnd_, msg, wp, lp);
                if (r != 0) return r;
                p->rgrc[0].top = top;
                if (::IsZoomed(hwnd_))
                    p->rgrc[0].top += ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi_) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            const LRESULT hit = ::DefWindowProcW(hwnd_, msg, wp, lp);
            if (hit != HTCLIENT) return hit;
            return HitTest(lp);
        }
        case WM_NCMOUSEMOVE: {
            const CaptionButton h = nc(wp);
            if (h != shellState_.hover) {
                shellState_.hover = h;
                Invalidate();
            }
            if (input_.inside) {
                input_.inside = false;
                Invalidate();
            }
            if (!trackingNc_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
                trackingNc_ = ::TrackMouseEvent(&tme) != FALSE;
            }
            if (h != CaptionButton::None) return 0;
            break;
        }
        case WM_NCMOUSELEAVE:
            trackingNc_ = false;
            shellState_.hover = CaptionButton::None;
            shellState_.pressed = CaptionButton::None;
            Invalidate();
            break;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (nc(wp) != CaptionButton::None) {
                shellState_.pressed = nc(wp);
                Invalidate();
                return 0;
            }
            break;
        case WM_NCLBUTTONUP: {
            const CaptionButton b = nc(wp);
            if (b != CaptionButton::None) {
                if (b == shellState_.pressed) {
                    if (b == CaptionButton::Minimize) ::ShowWindow(hwnd_, SW_MINIMIZE);
                    else if (b == CaptionButton::Maximize) ::ShowWindow(hwnd_, ::IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
                    else ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                }
                shellState_.pressed = CaptionButton::None;
                shellState_.hover = CaptionButton::None;
                Invalidate();
                return 0;
            }
            break;
        }
        case WM_NCRBUTTONUP:
            if (wp == HTCAPTION) {
                const HMENU menu = ::GetSystemMenu(hwnd_, FALSE);
                const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0,
                                                  hwnd_, nullptr);
                if (cmd) ::PostMessageW(hwnd_, WM_SYSCOMMAND, cmd, 0);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            SetMouse(lp, false);
            input_.inside = true;
            shellState_.hover = CaptionButton::None;
            if (!trackingMouse_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                trackingMouse_ = ::TrackMouseEvent(&tme) != FALSE;
            }
            Invalidate();
            return 0;
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            if (!input_.down) input_.inside = false;
            Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetMouse(lp, false);
            input_.inside = true;
            input_.down = true;
            input_.pressed = true;
            if (msg == WM_LBUTTONDBLCLK) input_.doubleClick = true;
            ::SetCapture(hwnd_);
            Invalidate();
            return 0;
        case WM_LBUTTONUP:
            SetMouse(lp, false);
            input_.down = false;
            input_.released = true;
            ::ReleaseCapture();
            Invalidate();
            return 0;
        case WM_RBUTTONUP:
            SetMouse(lp, false);
            input_.rightReleased = true;
            Invalidate();
            return 0;
        case WM_MOUSEWHEEL:
            SetMouse(lp, true);
            input_.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            Invalidate();
            return 0;
        case WM_KEYDOWN:
            input_.ctrl = ::GetKeyState(VK_CONTROL) < 0;
            input_.shift = ::GetKeyState(VK_SHIFT) < 0;
            input_.keys.push_back(static_cast<unsigned>(wp));
            Invalidate();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                ::SetCursor(::LoadCursorW(nullptr, ui_.Cursor() ? ui_.Cursor() : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_SIZE:
            shellState_.maximized = wp == SIZE_MAXIMIZED;
            renderer_.Resize(LOWORD(lp), HIWORD(lp));
            if (wp != SIZE_MINIMIZED) Render();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            ::BeginPaint(hwnd_, &ps);
            ::EndPaint(hwnd_, &ps);
            Invalidate();
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            inSizeMove_ = true;
            ::SetTimer(hwnd_, kTimerAnim, 10, nullptr);
            break;
        case WM_EXITSIZEMOVE:
            inSizeMove_ = false;
            ::KillTimer(hwnd_, kTimerAnim);
            Invalidate();
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wp == kTimerTick) {
                model_.now = Now();
                if (model_.Tick()) Invalidate();
            } else if (wp == kTimerAnim) {
                if (inSizeMove_ ? (dirty_ || animating_) : true) Render();
            } else if (wp == kTimerSmoke) {
                SmokeStep();
            } else if (wp == kTimerDevices) {
                ::KillTimer(hwnd_, kTimerDevices);
                model_.RefreshEndpoints();
                Invalidate();
            }
            return 0;
        case kMsgNotify:
            model_.now = Now();
            model_.Tick();
            Invalidate();
            return 0;
        case kMsgDevices:
            ::SetTimer(hwnd_, kTimerDevices, 800, nullptr);
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const auto* r = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Invalidate();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = static_cast<LONG>(980 * Scale());
            mmi->ptMinTrackSize.y = static_cast<LONG>(640 * Scale());
            return 0;
        }
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplyTheme();
            break;
        case WM_ACTIVATE:
            shellState_.active = LOWORD(wp) != WA_INACTIVE;
            Invalidate();
            break;
        case WM_CLOSE:
            ::DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            ::KillTimer(hwnd_, kTimerTick);
            ::PostQuitMessage(0);
            return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::SmokeStep() {
    // Capture the page shown since the previous step, then switch to the next one.
    if (smokePage_ >= 0 && !smokeDir_.empty()) {
        constexpr const wchar_t* kNames[] = {L"bridge", L"filter", L"tone", L"plugins", L"devices", L"log"};
        if (Capture(std::format(L"{}\\window_{}.png", smokeDir_, kNames[smokePage_]))) ++captures_;
    }
    ++smokePage_;
    if (smokePage_ >= static_cast<int>(Page::Count) || Now() > smokeSeconds_) {
        ::KillTimer(hwnd_, kTimerSmoke);
        ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }
    model_.page = static_cast<Page>(smokePage_);
    Invalidate();
}

bool MainWindow::Capture(const std::wstring& path) {
    RECT rc{};
    ::GetWindowRect(hwnd_, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = ::SelectObject(mem, bmp);
    const bool ok = ::PrintWindow(hwnd_, mem, PW_RENDERFULLCONTENT) != FALSE;
    ::SelectObject(mem, old);
    bool saved = false;
    if (ok && bits) {
        auto* px = static_cast<uint32_t*>(bits);
        for (int i = 0; i < w * h; ++i) px[i] |= 0xFF000000u;  // GDI leaves alpha undefined
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> wbmp;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        const UINT stride = static_cast<UINT>(w) * 4;
        saved = SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, stride, stride * h,
                                                      static_cast<BYTE*>(bits), &wbmp)) &&
                SUCCEEDED(wic->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                SUCCEEDED(frame->WriteSource(wbmp.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
                SUCCEEDED(enc->Commit());
    }
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return saved;
}

}  // namespace dgmod::app
