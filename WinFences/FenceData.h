#pragma once
// ============================================================================
// FenceData.h — Plain data types shared across all modules.
//
// FenceData        — persistent state of one fence (grid position + icons)
// FenceIconEntry   — one icon slot inside a fence (path, display name, etc.)
//
// IMPORTANT: FenceData contains NO pixel values. Position and size are always
// expressed as grid cell indices (col, row, cols, rows). Pixels are derived
// on demand via GridSystem.h. This is the single source of truth.
// ============================================================================
#include <string>
#include <vector>
#include <algorithm>
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>

// ── Data structs ─────────────────────────────────────────────────────────────

struct FenceIconEntry
{
    std::wstring path;
    std::wstring parsingName;
    std::wstring displayName;
    std::wstring iconKey;   // cache key
};

struct FenceData
{
    std::wstring                id;
    std::wstring                label;
    int                         col  = 0; // desktop grid position
    int                         row  = 0;
    int                         cols = 1; // size in cells — 1x1 minimum, set at creation
    int                         rows = 1;
    float                       alpha = 0.65f; // background opacity
    std::vector<FenceIconEntry> icons;
};

// ── Helpers ──────────────────────────────────────────────────────────────────

inline void SortIconsByName(std::vector<FenceIconEntry>& icons)
{
    std::sort(icons.begin(), icons.end(),
        [](const FenceIconEntry& a, const FenceIconEntry& b){
            return _wcsicmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
        });
}



inline std::wstring GetDisplayName(const std::wstring& path)
{
    wchar_t buf[MAX_PATH] = {};
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME))
        return sfi.szDisplayName;
    // fallback: filename without extension
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = (slash != std::wstring::npos) ? path.substr(slash+1) : path;
    size_t dot = name.rfind(L'.');
    return (dot != std::wstring::npos) ? name.substr(0, dot) : name;
}

// ── Icon size query ───────────────────────────────────────────────────────────

inline int GetDesktopIconSizePx(float scale, bool force = false)
{
    static float s_scale = 0.f;
    static int   s_result = 0;
    if (!force && s_scale == scale && s_result > 0) return s_result;

    int result = 0;
    IShellWindows* psw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
            IID_IShellWindows, reinterpret_cast<void**>(&psw))) && psw)
    {
        VARIANT vEmpty = {}; vEmpty.vt = VT_EMPTY;
        IDispatch* pDisp = nullptr; long hw = 0;
        if (SUCCEEDED(psw->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP,
                &hw, SWFO_NEEDDISPATCH, &pDisp)) && pDisp)
        {
            IServiceProvider* psp = nullptr;
            if (SUCCEEDED(pDisp->QueryInterface(IID_IServiceProvider,
                    reinterpret_cast<void**>(&psp))) && psp)
            {
                IShellBrowser* psb = nullptr;
                if (SUCCEEDED(psp->QueryService(SID_STopLevelBrowser,
                        IID_IShellBrowser, reinterpret_cast<void**>(&psb))) && psb)
                {
                    IShellView* psv = nullptr;
                    if (SUCCEEDED(psb->QueryActiveShellView(&psv)) && psv)
                    {
                        IFolderView2* pfv2 = nullptr;
                        if (SUCCEEDED(psv->QueryInterface(IID_IFolderView2,
                                reinterpret_cast<void**>(&pfv2))) && pfv2)
                        {
                            FOLDERVIEWMODE fvm = FVM_ICON; int sz = 0;
                            if (SUCCEEDED(pfv2->GetViewModeAndIconSize(&fvm, &sz)) && sz > 0)
                            {
                                // GetViewModeAndIconSize returns logical pixels (96-DPI).
                                // Multiply by scale to get physical pixels for icon bitmaps.
                                result = static_cast<int>(sz * scale);
                                DebugLog(L"[IconSize] sz=%d scale=%.2f result=%d", sz, scale, result);
                            }
                            pfv2->Release();
                        }
                        psv->Release();
                    }
                    psb->Release();
                }
                psp->Release();
            }
            pDisp->Release();
        }
        psw->Release();
    }
    if (result > 0) { s_scale = scale; s_result = result; }
    return result > 0 ? result : static_cast<int>(48.f * scale);
}

inline int GetIconLogicalPx(float scale, bool force = false)
{
    return static_cast<int>(GetDesktopIconSizePx(scale, force) / scale);
}

// ── Grid metrics (for rendering) ─────────────────────────────────────────────






