#pragma once
// ============================================================================
// Renderer.h — Direct2D + DirectWrite render target wrapper.
//
// Wraps ID2D1HwndRenderTarget with dpiX=dpiY=96 (raw physical pixel mode).
// All D2D coordinates are therefore PHYSICAL PIXELS — not logical DIPs.
// Font sizes passed to DWrite are also in physical pixels.
//
// One Renderer per FenceWindow. Call Resize() on WM_SIZE.
// ============================================================================
#include "pch.h"
#include "DpiHelper.h"

// Renderer using ID2D1HwndRenderTarget - simple, stable, no DComposition needed.
// Transparency is handled via WS_EX_LAYERED + per-pixel alpha (ARGB).
// VSync: HwndRenderTarget with D2D1_PRESENT_OPTIONS_NONE syncs to DWM on Win10+.
class Renderer
{
public:
    Renderer() = default;
    ~Renderer() { ReleaseResources(); }

    HRESULT Initialize(HWND hwnd)
    {
        m_hwnd = hwnd;
        HRESULT hr = CreateDeviceIndependentResources();
        if (SUCCEEDED(hr))
            hr = CreateDeviceResources();
        return hr;
    }

    HRESULT Resize()
    {
        if (!m_renderTarget) return S_FALSE;
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(
            imax(1L, rc.right  - rc.left),
            imax(1L, rc.bottom - rc.top));
        return m_renderTarget->Resize(size);
    }

    void BeginDraw()
    {
        m_renderTarget->BeginDraw();
        m_renderTarget->Clear(D2D1::ColorF(0, 0.0f));
    }

    HRESULT EndDraw()
    {
        HRESULT hr = m_renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
        {
            ReleaseDeviceResources();
            hr = CreateDeviceResources();
        }
        return hr;
    }

    // DC() - ID2D1RenderTarget interface for brush/bitmap/text operations
    ID2D1RenderTarget* DC()      const { return m_renderTarget.Get(); }
    ID2D1Factory*      Factory() const { return m_d2dFactory.Get(); }
    IDWriteFactory3*   DWrite()  const { return m_dwriteFactory.Get(); }
    IWICImagingFactory* WIC()   const { return m_wicFactory.Get(); }
    bool IsReady()               const { return m_renderTarget != nullptr; }

    HRESULT CreateBitmapFromHIcon(HICON hIcon, ID2D1Bitmap** ppBitmap)
    {
        if (!m_wicFactory || !m_renderTarget) return E_FAIL;
        ComPtr<IWICBitmap> wicBitmap;
        HRESULT hr = m_wicFactory->CreateBitmapFromHICON(hIcon, &wicBitmap);
        if (FAILED(hr)) return hr;
        return ConvertAndCreate(wicBitmap.Get(), ppBitmap);
    }

    HRESULT CreateBitmapFromHBITMAP(HBITMAP hbm, ID2D1Bitmap** ppBitmap)
    {
        if (!m_wicFactory || !m_renderTarget) return E_FAIL;
        ComPtr<IWICBitmap> wicBitmap;
        // WICBitmapUseAlpha = treat source as straight alpha (correct for Shell HBITMAPs).
        // The FormatConverter below converts to premultiplied PBGRA properly,
        // avoiding the fringe/fuzz artifacts at icon edges.
        HRESULT hr = m_wicFactory->CreateBitmapFromHBITMAP(
            hbm, nullptr, WICBitmapUseAlpha, &wicBitmap);
        if (FAILED(hr)) return hr;
        return ConvertAndCreate(wicBitmap.Get(), ppBitmap);
    }

private:
    HWND m_hwnd = nullptr;

    ComPtr<ID2D1Factory>           m_d2dFactory;
    ComPtr<ID2D1HwndRenderTarget>  m_renderTarget;
    ComPtr<IDWriteFactory3>         m_dwriteFactory;
    ComPtr<IWICImagingFactory>      m_wicFactory;

    HRESULT ConvertAndCreate(IWICBitmapSource* source, ID2D1Bitmap** ppBitmap)
    {
        ComPtr<IWICFormatConverter> converter;
        HRESULT hr = m_wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return hr;
        hr = converter->Initialize(source,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeMedianCut);
        if (FAILED(hr)) return hr;
        return m_renderTarget->CreateBitmapFromWicBitmap(converter.Get(), ppBitmap);
    }

    HRESULT CreateDeviceIndependentResources()
    {
        D2D1_FACTORY_OPTIONS opts = {};
#ifdef _DEBUG
        opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory),
            &opts,
            reinterpret_cast<void**>(m_d2dFactory.GetAddressOf()));
        if (FAILED(hr)) return hr;

        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory3),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
        if (FAILED(hr)) return hr;

        return CoCreateInstance(
            CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(m_wicFactory.GetAddressOf()));
    }

    HRESULT CreateDeviceResources()
    {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(
            imax(1L, rc.right  - rc.left),
            imax(1L, rc.bottom - rc.top));

        // dpiX/dpiY = 96: D2D uses raw pixel coordinates (1 unit = 1 physical pixel).
        // Do NOT use 0 here - that makes D2D read system DPI (96) and treat our
        // physical-pixel coordinates as logical units, clipping at 1/scale of the window.
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);

        D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps =
            D2D1::HwndRenderTargetProperties(m_hwnd, size, D2D1_PRESENT_OPTIONS_NONE);

        return m_d2dFactory->CreateHwndRenderTarget(
            rtProps, hwndProps, m_renderTarget.GetAddressOf());
    }

    void ReleaseDeviceResources() { m_renderTarget.Reset(); }

    void ReleaseResources()
    {
        ReleaseDeviceResources();
        m_d2dFactory.Reset();
        m_dwriteFactory.Reset();
        m_wicFactory.Reset();
    }
};
