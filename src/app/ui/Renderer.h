#pragma once

#include "common/Win.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <wincodec.h>

namespace dgmod::ui {

// Process-wide device-independent factories.
struct Factories {
    ComPtr<ID2D1Factory1> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory2> wic;
    static Factories& Get();
};

// D3D11 + DXGI composition swap chain + DirectComposition + D2D device context.
// Transparent (premultiplied) output lets the DWM Mica backdrop show through.
class Renderer {
public:
    Result<void> InitForWindow(HWND hwnd);
    Result<void> InitOffscreen();
    void Resize(UINT width, UINT height);

    // Begins drawing into the swap chain back buffer. Returns nullptr if the device is unavailable.
    ID2D1DeviceContext* BeginDraw(float dpi);
    // Ends drawing and presents. Handles device loss by recreating resources on the next frame.
    void EndDraw();

    // Offscreen: render into a bitmap of the given pixel size and save as PNG.
    ID2D1DeviceContext* BeginOffscreen(UINT width, UINT height, float dpi);
    Result<void> EndOffscreenAndSave(const std::wstring& path);

    [[nodiscard]] ID2D1DeviceContext* Context() const { return dc_.Get(); }
    [[nodiscard]] bool Ready() const { return swapChain_ != nullptr || deviceLost_; }
    // Signaled by DXGI when the compositor can accept the next frame (paced at the monitor refresh rate).
    [[nodiscard]] HANDLE FrameWaitable() const { return waitable_; }
    ~Renderer();

private:
    Result<void> CreateDevice();
    Result<void> CreateSwapChain();
    void ReleaseDevice();

    HWND hwnd_ = nullptr;
    UINT width_ = 1, height_ = 1;
    bool deviceLost_ = false;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGIDevice> dxgi_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<IDXGISwapChain1> swapChain_;
    HANDLE waitable_ = nullptr;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1Bitmap1> offscreen_;
};

}  // namespace dgmod::ui
