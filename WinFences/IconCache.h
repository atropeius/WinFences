#pragma once
// ============================================================================
// IconCache.h — Shell icon loader and D2D bitmap cache.
//
// Loads shell icons from filesystem paths using IShellItemImageFactory
// (256px, strategy 1) with SHIL_JUMBO fallback (strategy 2/3).
//
// Shell overlays (the shortcut arrow, OneDrive sync badges, the sharing hand)
// are NOT baked into the cached icon. They are cached separately and drawn as a
// second, scaled-down pass in DrawIcon() — see OVERLAY_SCALE for why.
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
    // Edge length of a shell overlay badge, as a fraction of the icon's edge.
    //
    // Letting the image list composite the badge (ILD_TRANSPARENT |
    // INDEXTOOVERLAYMASK) draws it at ~39% of the icon edge, which reads as a
    // huge arrow at fence icon sizes. Drawing it ourselves at 0.5 halves it.
    // The overlay image is a full-size canvas with the badge in its bottom-left
    // corner, so shrinking the whole canvas against the bottom-left corner
    // keeps the badge in place and only scales it.
    static constexpr float OVERLAY_SCALE = 0.5f;

    // Upper bound for a requested icon size. Nothing in a fence is drawn this
    // large; it only caps pathological cases.
    static constexpr int ICON_LOAD_PX = 256;

    // A Renderer is only needed for the WIC factory (device-independent).
    // We keep a pointer to any one renderer just for WIC access.
    void SetRenderer(Renderer* r) { m_renderer = r; }

    void Invalidate(const std::wstring& path)
    {
        m_overlayIdx.erase(path);

        // Cache keys are "<path>|<size>", so one path can hold several entries.
        const std::wstring prefix = path + L'|';
        for (auto it = m_wicCache.begin(); it != m_wicCache.end(); )
            it = it->first.starts_with(prefix) ? m_wicCache.erase(it) : std::next(it);
        for (auto it = m_d2dCache.begin(); it != m_d2dCache.end(); )
            it = it->first.first.starts_with(prefix) ? m_d2dCache.erase(it) : std::next(it);
    }

    void Clear()
    {
        m_wicCache.clear();
        m_d2dCache.clear();
        m_overlayIdx.clear();
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
        // Ask the shell for the size we are about to draw, the way Explorer
        // does, instead of always asking for 256. Requesting more than an item
        // actually has is what produces a small icon marooned in a big empty
        // frame; requesting the display size gets the variant the icon author
        // drew for roughly that size, so it stays sharp and fills its slot.
        const int px = RequestSizeFor(destRect);

        ID2D1Bitmap* bmp = GetD2DBitmap(dc, path, px);
        if (!bmp) return;
        dc->DrawBitmap(bmp, destRect, opacity,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        // Overlay badge on top, shrunk against the bottom-left corner.
        int overlayIdx = OverlayIndexFor(path);
        if (overlayIdx == 0) return;

        ID2D1Bitmap* badge = GetD2DBitmap(dc, OverlayKey(overlayIdx), px);
        if (!badge) return;

        float w = (destRect.right  - destRect.left) * OVERLAY_SCALE;
        float h = (destRect.bottom - destRect.top ) * OVERLAY_SCALE;
        dc->DrawBitmap(badge,
            D2D1::RectF(destRect.left, destRect.bottom - h,
                        destRect.left + w, destRect.bottom),
            opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

private:
    Renderer* m_renderer = nullptr;

    // Overlay images live in the same caches as icons, under a key that starts
    // with a character no filesystem path or shell parsing name can contain.
    static constexpr wchar_t OVERLAY_KEY_PREFIX = L'\x01';
    static std::wstring OverlayKey(int overlayIdx)
    {
        return std::wstring(1, OVERLAY_KEY_PREFIX) + L"ovl"
             + std::to_wstring(overlayIdx);
    }

    // path -> shell overlay index (0 = none). Memoised: the lookup costs a
    // shell round trip, and Render() runs on every mouse move over a fence.
    std::unordered_map<std::wstring, int> m_overlayIdx;

    int OverlayIndexFor(const std::wstring& path)
    {
        if (path.empty() || path[0] == OVERLAY_KEY_PREFIX) return 0;

        auto it = m_overlayIdx.find(path);
        if (it != m_overlayIdx.end()) return it->second;

        int idx = GetOverlayIndex(path);
        m_overlayIdx[path] = idx;
        return idx;
    }

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

    // Icon size to request for a given destination rectangle: the drawn edge
    // length, rounded up to the next size Windows stores icons at. Rounding up
    // means a fence never scales an icon up by more than the gap between two
    // rungs of that ladder. Fences on one monitor share a size, so this adds at
    // most one cache entry per DPI in play.
    static int RequestSizeFor(const D2D1_RECT_F& r)
    {
        float edge = (r.right - r.left) > (r.bottom - r.top)
                   ? (r.right - r.left) : (r.bottom - r.top);
        int px = static_cast<int>(edge + 0.5f);
        if (px < 16) px = 16;
        if (px > ICON_LOAD_PX) px = ICON_LOAD_PX;
        return RoundUpToStandardIconSize(px);
    }

    static std::wstring CacheKey(const std::wstring& path, int px)
    {
        return path + L'|' + std::to_wstring(px);
    }

    // Get or create a D2D bitmap for the given render target
    ID2D1Bitmap* GetD2DBitmap(ID2D1RenderTarget* dc, const std::wstring& path, int px)
    {
        D2DKey key = { CacheKey(path, px), dc };
        auto it = m_d2dCache.find(key);
        if (it != m_d2dCache.end())
            return it->second.Get();

        // Get WIC bitmap (load if needed)
        IWICBitmap* wic = GetWICBitmap(key.first, path, px);
        if (!wic) return nullptr;

        // Convert WIC to D2D bitmap for THIS render target
        ComPtr<ID2D1Bitmap> d2dBmp;
        if (FAILED(CreateD2DBitmapFromWIC(dc, wic, &d2dBmp)))
            return nullptr;

        m_d2dCache[key] = d2dBmp;
        return d2dBmp.Get();
    }

    // Get or load WIC bitmap (device-independent)
    IWICBitmap* GetWICBitmap(const std::wstring& key, const std::wstring& path, int px)
    {
        auto it = m_wicCache.find(key);
        if (it != m_wicCache.end())
            return it->second.Get();

        ComPtr<IWICBitmap> wic;
        if (SUCCEEDED(LoadWIC(path, px, &wic)))
        {
            m_wicCache[key] = wic;
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

    // Shell overlay index for a path, 0 when the item carries none.
    //
    // SHGFI_OVERLAYINDEX only reports anything when SHGFI_ICON is set as well —
    // with SHGFI_SYSICONINDEX alone the overlay bits are always 0 (verified: a
    // .lnk reports overlay 2 with SHGFI_ICON, 0 without). We therefore ask for
    // the small icon, the cheapest one for the shell to build, and discard it;
    // only the index bits 24-27 of iIcon are wanted.
    static int GetOverlayIndex(const std::wstring& path)
    {
        SHFILEINFOW sfi = {};
        if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                SHGFI_ICON | SHGFI_SMALLICON | SHGFI_OVERLAYINDEX))
            return 0;
        if (sfi.hIcon) DestroyIcon(sfi.hIcon);
        return (sfi.iIcon >> 24) & 0x0F;
    }

    // The overlay artwork on its own, straight from the system image list, so
    // the badge is whatever the running Windows version uses. It comes back as
    // a full-size canvas with the badge in the bottom-left corner.
    HRESULT LoadOverlayImage(int overlayIdx, IWICImagingFactory* wicFactory,
                             IWICBitmap** ppWic)
    {
        // Jumbo first (256px, matches the icon cache size), extra-large as a
        // fallback for shells that have no jumbo overlay artwork.
        for (int shil : { SHIL_JUMBO, SHIL_EXTRALARGE })
        {
            IImageList* pIml = nullptr;
            if (FAILED(SHGetImageList(shil, IID_IImageList,
                    reinterpret_cast<void**>(&pIml))) || !pIml)
                continue;

            int imgIdx = -1;
            HRESULT hr = pIml->GetOverlayImage(overlayIdx, &imgIdx);

            HICON hIcon = nullptr;
            if (SUCCEEDED(hr) && imgIdx >= 0)
                hr = pIml->GetIcon(imgIdx, ILD_TRANSPARENT, &hIcon);
            pIml->Release();

            if (SUCCEEDED(hr) && hIcon)
            {
                hr = wicFactory->CreateBitmapFromHICON(hIcon, ppWic);
                DestroyIcon(hIcon);
                if (SUCCEEDED(hr)) return hr;
            }
        }
        return E_FAIL;
    }

    // Longest edge of the non-transparent region, in pixels, or 0 when it
    // cannot be determined. Measured through WIC on purpose: GetDIBits does
    // not hand back the alpha channel of these bitmaps — it reports every
    // pixel opaque, which makes padding undetectable.
    static int MeasureContentExtent(IWICBitmap* bmp)
    {
        if (!bmp) return 0;
        UINT w = 0, h = 0;
        if (FAILED(bmp->GetSize(&w, &h)) || !w || !h) return 0;

        WICPixelFormatGUID fmt;
        if (FAILED(bmp->GetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat32bppBGRA)
            return 0;

        WICRect rc = { 0, 0, static_cast<INT>(w), static_cast<INT>(h) };
        ComPtr<IWICBitmapLock> lock;
        if (FAILED(bmp->Lock(&rc, WICBitmapLockRead, &lock))) return 0;

        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        if (FAILED(lock->GetStride(&stride))
            || FAILED(lock->GetDataPointer(&size, &data)) || !data)
            return 0;

        int minX = static_cast<int>(w), minY = static_cast<int>(h), maxX = -1, maxY = -1;
        for (UINT y = 0; y < h; ++y)
        {
            const BYTE* row = data + static_cast<size_t>(y) * stride;
            for (UINT x = 0; x < w; ++x)
                if (row[x * 4 + 3] > 8)
                {
                    if (static_cast<int>(x) < minX) minX = static_cast<int>(x);
                    if (static_cast<int>(y) < minY) minY = static_cast<int>(y);
                    if (static_cast<int>(x) > maxX) maxX = static_cast<int>(x);
                    if (static_cast<int>(y) > maxY) maxY = static_cast<int>(y);
                }
        }

        if (maxX < 0) return 0; // nothing opaque — leave the caller's image alone
        return imax(maxX - minX + 1, maxY - minY + 1);
    }

    // Shell item for a shortcut's target, when the target's icon is the one to
    // use. Returns false for anything that is not a .lnk, for a shortcut that
    // carries an icon of its own, and when the target cannot be resolved.
    //
    // Needed because the shell can fail to produce an icon for a .lnk whose
    // target is a packaged (Store) app: asking about the shortcut returns the
    // generic blank-document icon while the target resolves fine and has a
    // perfectly good one. Verified with a WhatsApp shortcut — blank page via the
    // .lnk, the real logo via its AppsFolder target. Explorer shows the target's
    // icon for a shortcut anyway, so asking the target is also the truer answer;
    // the arrow overlay is drawn separately and is unaffected.
    static bool ResolveLinkTarget(const std::wstring& path, ComPtr<IShellItem>& out)
    {
        if (path.size() <= 4
            || _wcsicmp(path.c_str() + path.size() - 4, L".lnk") != 0)
            return false;

        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&link))))
            return false;

        ComPtr<IPersistFile> pf;
        if (FAILED(link.As(&pf)) || FAILED(pf->Load(path.c_str(), STGM_READ)))
            return false;

        // An icon set on the shortcut itself must win over the target's.
        wchar_t iconPath[MAX_PATH] = {};
        int iconIdx = 0;
        if (SUCCEEDED(link->GetIconLocation(iconPath, MAX_PATH, &iconIdx))
            && iconPath[0] != L'\0')
            return false;

        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(link->GetIDList(&pidl)) || !pidl) return false;

        HRESULT hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&out));
        CoTaskMemFree(pidl);
        return SUCCEEDED(hr) && out;
    }

    // One GetImage call, wrapped as a WIC bitmap.
    static HRESULT LoadViaImageFactory(IShellItemImageFactory* factory, int px,
                                       IWICImagingFactory* wicFactory,
                                       IWICBitmap** ppWic)
    {
        HBITMAP hbm = nullptr;
        SIZE sz = { px, px };
        HRESULT hr = factory->GetImage(sz, SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY, &hbm);
        if (FAILED(hr) || !hbm) return FAILED(hr) ? hr : E_FAIL;

        hr = wicFactory->CreateBitmapFromHBITMAP(hbm, nullptr, WICBitmapUseAlpha, ppWic);
        DeleteObject(hbm);
        return hr;
    }

    // Next size up in the ladder Windows stores icons at.
    static int RoundUpToStandardIconSize(int px)
    {
        for (int s : { 16, 20, 24, 32, 40, 48, 64, 96, 128, 256 })
            if (px <= s) return s;
        return ICON_LOAD_PX;
    }

    // Load an icon as a WIC bitmap at `px`, device-independent.
    HRESULT LoadWIC(const std::wstring& path, int px, IWICBitmap** ppWic)
    {
        if (!m_renderer) return E_FAIL;
        IWICImagingFactory* wicFactory = m_renderer->WIC();
        if (!wicFactory) return E_FAIL;

        // Synthetic key: this is an overlay badge, not a file.
        if (!path.empty() && path[0] == OVERLAY_KEY_PREFIX)
        {
            int overlayIdx = _wtoi(path.c_str() + 4); // skip "\x01ovl"
            return LoadOverlayImage(overlayIdx, wicFactory, ppWic);
        }

        // Strategy 1: IShellItemImageFactory -> HBITMAP -> WIC
        //
        // Two candidates, in order: a shortcut's target (see ResolveLinkTarget)
        // and the item itself. The target is tried first because the shell can
        // fail to give a .lnk an icon at all; the item is the fallback and the
        // only candidate for everything that is not a shortcut.
        {
            ComPtr<IShellItem> candidates[2];
            int count = 0;
            if (ResolveLinkTarget(path, candidates[count])) ++count;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    path.c_str(), nullptr, IID_PPV_ARGS(&candidates[count]))))
                ++count;

            for (int c = 0; c < count; ++c)
            {
                ComPtr<IShellItemImageFactory> factory;
                if (SUCCEEDED(candidates[c].As(&factory)))
                {
                    ComPtr<IWICBitmap> loaded;
                    if (SUCCEEDED(LoadViaImageFactory(factory.Get(), px,
                            wicFactory, &loaded)) && loaded)
                    {
                        // SIIGBF_RESIZETOFIT only ever shrinks. An item whose
                        // largest icon is smaller than the requested size comes
                        // back centred at 1:1 in an otherwise empty canvas, and
                        // then draws as a tiny picture floating in a big frame.
                        // Requesting the display size makes this rare, but it
                        // still happens below 32px items; drop to the real size
                        // so the art fills its frame either way.
                        int extent = MeasureContentExtent(loaded.Get());
                        if (extent > 0 && extent < px * 3 / 4)
                        {
                            int native = RoundUpToStandardIconSize(extent);
                            ComPtr<IWICBitmap> tight;
                            if (native < px
                                && SUCCEEDED(LoadViaImageFactory(factory.Get(),
                                       native, wicFactory, &tight)) && tight)
                                loaded = tight;
                        }

                        *ppWic = loaded.Detach();
                        return S_OK;
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
