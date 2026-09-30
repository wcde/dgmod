#include "ui/Renderer.h"

namespace dgmod::ui {

Factories& Factories::Get() {
    static Factories f = [] {
        Factories r;
        D2D1_FACTORY_OPTIONS opts{};
        ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                            reinterpret_cast<void**>(r.d2d.GetAddressOf()));
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(r.dwrite.GetAddressOf()));
        ::CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&r.wic));
        return r;
    }();
    return f;
}

Result<void> Renderer::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d_, nullptr,
                                     nullptr);
    if (FAILED(hr))
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                 static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
    if (FAILED(hr)) return Fail(hr, L"D3D11CreateDevice");
    hr = d3d_.As(&dxgi_);
    if (FAILED(hr)) return Fail(hr, L"IDXGIDevice");
    auto& f = Factories::Get();
    if (!f.d2d) return Fail(E_FAIL, L"D2D1CreateFactory");
    hr = f.d2d->CreateDevice(dxgi_.Get(), &d2dDevice_);
    if (FAILED(hr)) return Fail(hr, L"ID2D1Device");
    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
    if (FAILED(hr)) return Fail(hr, L"ID2D1DeviceContext");
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc_->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    return {};
}

Result<void> Renderer::CreateSwapChain() {
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Fail(hr, L"CreateDXGIFactory2");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Three buffers: with two, a composition swap chain can only be flipped every other compositor frame
    // (the buffer DWM is scanning out is released one frame late), which halves the frame rate.
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    hr = factory->CreateSwapChainForComposition(dxgi_.Get(), &desc, nullptr, &swapChain_);
    if (FAILED(hr)) return Fail(hr, L"CreateSwapChainForComposition");
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swapChain_.As(&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        waitable_ = sc2->GetFrameLatencyWaitableObject();
    }

    hr = ::DCompositionCreateDevice(dxgi_.Get(), IID_PPV_ARGS(&dcomp_));
    if (FAILED(hr)) return Fail(hr, L"DCompositionCreateDevice");
    hr = dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_);
    if (FAILED(hr)) return Fail(hr, L"CreateTargetForHwnd");
    hr = dcomp_->CreateVisual(&visual_);
    if (FAILED(hr)) return Fail(hr, L"CreateVisual");
    visual_->SetContent(swapChain_.Get());
    dcompTarget_->SetRoot(visual_.Get());
    dcomp_->Commit();
    return {};
}

Renderer::~Renderer() {
    if (waitable_) ::CloseHandle(waitable_);
}

void Renderer::ReleaseDevice() {
    if (waitable_) {
        ::CloseHandle(waitable_);
        waitable_ = nullptr;
    }
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    offscreen_.Reset();
    visual_.Reset();
    dcompTarget_.Reset();
    dcomp_.Reset();
    swapChain_.Reset();
    dc_.Reset();
    d2dDevice_.Reset();
    dxgi_.Reset();
    d3d_.Reset();
}

Result<void> Renderer::InitForWindow(HWND hwnd) {
    hwnd_ = hwnd;
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    width_ = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    height_ = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    if (auto r = CreateDevice(); !r) return r;
    return CreateSwapChain();
}

Result<void> Renderer::InitOffscreen() { return CreateDevice(); }

void Renderer::Resize(UINT width, UINT height) {
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    if (!swapChain_) return;
    dc_->SetTarget(nullptr);
    target_.Reset();
    const HRESULT hr = swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN,
                                                 DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) deviceLost_ = true;
}

ID2D1DeviceContext* Renderer::BeginDraw(float dpi) {
    if (deviceLost_) {
        ReleaseDevice();
        deviceLost_ = false;
        if (!CreateDevice() || !CreateSwapChain()) {
            deviceLost_ = true;
            return nullptr;
        }
    }
    if (!swapChain_) return nullptr;
    if (!target_) {
        ComPtr<IDXGISurface> surface;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
            deviceLost_ = true;
            return nullptr;
        }
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) {
            deviceLost_ = true;
            return nullptr;
        }
    }
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi, dpi);
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    return dc_.Get();
}

void Renderer::EndDraw() {
    HRESULT hr = dc_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        deviceLost_ = true;
        return;
    }
    hr = swapChain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) deviceLost_ = true;
}

ID2D1DeviceContext* Renderer::BeginOffscreen(UINT width, UINT height, float dpi) {
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi,
        dpi);
    offscreen_.Reset();
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &props, &offscreen_))) return nullptr;
    dc_->SetTarget(offscreen_.Get());
    dc_->SetDpi(dpi, dpi);
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    return dc_.Get();
}

Result<void> Renderer::EndOffscreenAndSave(const std::wstring& path) {
    HRESULT hr = dc_->EndDraw();
    if (FAILED(hr)) return Fail(hr, L"EndDraw");
    auto& f = Factories::Get();
    if (!f.wic) return Fail(E_FAIL, L"WIC");
    ComPtr<IWICImageEncoder> imageEncoder;
    hr = f.wic->CreateImageEncoder(d2dDevice_.Get(), &imageEncoder);
    if (FAILED(hr)) return Fail(hr, L"CreateImageEncoder");
    ComPtr<IWICStream> stream;
    hr = f.wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return Fail(hr, L"WIC stream " + path);
    ComPtr<IWICBitmapEncoder> encoder;
    hr = f.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) {
        const D2D1_SIZE_U px = offscreen_->GetPixelSize();
        float dpiX = 96.f, dpiY = 96.f;
        offscreen_->GetDpi(&dpiX, &dpiY);
        WICImageParameters params{};
        params.PixelFormat = offscreen_->GetPixelFormat();
        params.DpiX = dpiX;
        params.DpiY = dpiY;
        params.PixelWidth = px.width;
        params.PixelHeight = px.height;
        hr = imageEncoder->WriteFrame(offscreen_.Get(), frame.Get(), &params);
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    dc_->SetTarget(nullptr);
    if (FAILED(hr)) return Fail(hr, L"PNG encode");
    return {};
}

}  // namespace dgmod::ui
