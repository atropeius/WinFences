#pragma once
// ============================================================================
// IconCache.h — Shell icon loader and D2D bitmap cache.
//
// Loads shell icons from filesystem paths using IShellItemImageFactory
// (256px, strategy 1) with SHIL_JUMBO fallback (strategy 2/3).
//
// Two cache levels:
//   WIC cache  — IWICBitmap per path (survives render target recreation)
//   D2D cache  — ID2D1Bitmap per (path, render target) pair
//
// DrawIcon(dc, path, destRect) — draws icon scaled to destRect (physical px).
// InvalidateRenderTarget(dc)   — call before destroying a render target.
// ============================================================================
#include "pch.h"
#include <commoncontrols.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include "Renderer.h"

// ---------------------------------------------------------------------------
// IconCache
// ---------------------------------------------------------------------------
// Two-level cache:
//   Level 1: path -> IWICBitmap (256px, device-independent, shared across fences)
//   Level 2: (path, ID2D1RenderTarget*) -> ID2D1Bitmap (per-fence D2D resource)
//
// This fixes the multi-fence crash: D2D bitmaps are bound to their factory.
// Each fence has its own D2D factory/render target. By caching WIC bitmaps at
// level 1 and converting to D2D on demand per render target at level 2,
// every fence gets its own D2D bitmap created from the same WIC source.
// ---------------------------------------------------------------------------

class IconCache
{
public:
    // A Renderer is only needed for the WIC factory (device-independent).
    // We keep a pointer to any one renderer just for WIC access.
    void SetRenderer(Renderer* r) { m_renderer = r; }

    void Invalidate(const std::wstring& path)
    {
        m_wicCache.erase(path);
        // Remove all D2D bitmaps for this path across all render targets
        for (auto it = m_d2dCache.begin(); it != m_d2dCache.end(); )
            it = (it->first.first == path) ? m_d2dCache.erase(it) : std::next(it);
    }

    void Clear()
    {
        m_wicCache.clear();
        m_d2dCache.clear();
    }

    // Call when a render target is destroyed to release its D2D bitmaps
    void InvalidateRenderTarget(ID2D1RenderTarget* rt)
    {
        for (auto it = m_d2dCache.begin(); it != m_d2dCache.end(); )
            it = (it->first.second == rt) ? m_d2dCache.erase(it) : std::next(it);
    }

    void DrawIcon(ID2D1RenderTarget* dc, const std::wstring& path,
                  const D2D1_RECT_F& destRect, float opacity = 1.0f)
    {
        ID2D1Bitmap* bmp = GetD2DBitmap(dc, path);
        if (!bmp) return;
        dc->DrawBitmap(bmp, destRect, opacity,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

private:
    Renderer* m_renderer = nullptr;

    // Level 1: device-independent WIC bitmaps (shared)
    std::unordered_map<std::wstring, ComPtr<IWICBitmap>> m_wicCache;

    // Level 2: per-(path, renderTarget) D2D bitmaps
    using D2DKey = std::pair<std::wstring, ID2D1RenderTarget*>;
    struct D2DKeyHash {
        size_t operator()(const D2DKey& k) const {
            return std::hash<std::wstring>()(k.first)
                 ^ (std::hash<void*>()(k.second) << 32);
        }
    };
    std::unordered_map<D2DKey, ComPtr<ID2D1Bitmap>, D2DKeyHash> m_d2dCache;

    // Get or create a D2D bitmap for the given render target
    ID2D1Bitmap* GetD2DBitmap(ID2D1RenderTarget* dc, const std::wstring& path)
    {
        D2DKey key = { path, dc };
        auto it = m_d2dCache.find(key);
        if (it != m_d2dCache.end())
            return it->second.Get();

        // Get WIC bitmap (load if needed)
        IWICBitmap* wic = GetWICBitmap(path);
        if (!wic) return nullptr;

        // Convert WIC to D2D bitmap for THIS render target
        ComPtr<ID2D1Bitmap> d2dBmp;
        if (FAILED(CreateD2DBitmapFromWIC(dc, wic, &d2dBmp)))
            return nullptr;

        m_d2dCache[key] = d2dBmp;
        return d2dBmp.Get();
    }

    // Get or load WIC bitmap (device-independent)
    IWICBitmap* GetWICBitmap(const std::wstring& path)
    {
        auto it = m_wicCache.find(path);
        if (it != m_wicCache.end())
            return it->second.Get();

        ComPtr<IWICBitmap> wic;
        if (SUCCEEDED(LoadWIC(path, &wic)))
        {
            m_wicCache[path] = wic;
            return wic.Get();
        }
        return nullptr;
    }

    // Convert WIC bitmap to D2D bitmap for a specific render target
    HRESULT CreateD2DBitmapFromWIC(ID2D1RenderTarget* dc,
                                    IWICBitmap* wic,
                                    ID2D1Bitmap** ppBitmap)
    {
        if (!m_renderer) return E_FAIL;
        IWICImagingFactory* wicFactory = m_renderer->WIC();
        if (!wicFactory) return E_FAIL;

        ComPtr<IWICFormatConverter> converter;
        HRESULT hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return hr;

        hr = converter->Initialize(wic,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeMedianCut);
        if (FAILED(hr)) return hr;

        return dc->CreateBitmapFromWicBitmap(converter.Get(), ppBitmap);
    }

    // Load icon as WIC bitmap (256px, device-independent)
    HRESULT LoadWIC(const std::wstring& path, IWICBitmap** ppWic)
    {
        if (!m_renderer) return E_FAIL;
        IWICImagingFactory* wicFactory = m_renderer->WIC();
        if (!wicFactory) return E_FAIL;

        // Strategy 1: IShellItemImageFactory -> HBITMAP -> WIC
        {
            ComPtr<IShellItem> shellItem;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    path.c_str(), nullptr, IID_PPV_ARGS(&shellItem))))
            {
                ComPtr<IShellItemImageFactory> factory;
                if (SUCCEEDED(shellItem.As(&factory)))
                {
                    HBITMAP hbm = nullptr;
                    SIZE sz = { 256, 256 };
                    if (SUCCEEDED(factory->GetImage(sz,
                            SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY, &hbm)) && hbm)
                    {
                        HRESULT hr = wicFactory->CreateBitmapFromHBITMAP(
                            hbm, nullptr, WICBitmapUseAlpha, ppWic);
                        DeleteObject(hbm);
                        if (SUCCEEDED(hr)) return hr;
                    }
                }
            }
        }

        // Strategy 2: SHGetImageList SHIL_JUMBO -> HICON -> WIC
        {
            IImageList* pIml = nullptr;
            if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_IImageList,
                    reinterpret_cast<void**>(&pIml))) && pIml)
            {
                SHFILEINFOW sfi = {};
                SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX);
                HICON hIcon = nullptr;
                HRESULT hr = pIml->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon);
                pIml->Release();
                if (SUCCEEDED(hr) && hIcon)
                {
                    hr = wicFactory->CreateBitmapFromHICON(hIcon, ppWic);
                    DestroyIcon(hIcon);
                    if (SUCCEEDED(hr)) return hr;
                }
            }
        }

        // Strategy 3: SHGetFileInfo HICON -> WIC
        {
            SHFILEINFOW sfi = {};
            if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                    SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon)
            {
                HRESULT hr = wicFactory->CreateBitmapFromHICON(sfi.hIcon, ppWic);
                DestroyIcon(sfi.hIcon);
                return hr;
            }
        }

        return E_FAIL;
    }
};
