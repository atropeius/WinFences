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

// Accent colour a fence falls back to — the dark blue the app shipped with.
// Everything a fence paints (body, header bar, border) is derived from its
// accent colour, so one picked colour retints the whole fence coherently.
inline constexpr COLORREF FENCE_DEFAULT_COLOR = RGB(46, 56, 115);

struct FenceData
{
    std::wstring                id;
    std::wstring                label;
    int                         col  = 0; // desktop grid position
    int                         row  = 0;
    int                         cols = 1; // size in cells — 1x1 minimum, set at creation
    int                         rows = 1;
    float                       alpha = 0.65f; // background opacity
    COLORREF                    color = FENCE_DEFAULT_COLOR; // per-fence accent
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

// True for a real filesystem path (C:\... or \\UNC\...), false for virtual
// shell items such as ::{CLSID} or shell:AppsFolder\... .
inline bool IsFilesystemPath(const std::wstring& s)
{
    if (s.size() < 2) return false;
    if (s[0] == L':') return false;
    if (_wcsnicmp(s.c_str(), L"shell:", 6) == 0) return false;
    if (s[1] == L':') return true;                  // drive letter
    if (s[0] == L'\\' && s[1] == L'\\') return true; // UNC
    return false;
}

// Label under an icon and inside the expanded popup on selection — both read
// FenceIconEntry::displayName, so this is the single place that decides it.
//
// Rule: the plain file name, except that a shortcut's ".lnk" is dropped, so
// "Dokument.lnk" reads as "Dokument". Every other extension stays visible.
//
// Not GetDisplayName() for the general case on purpose: that one also hides
// ".txt" whenever Explorer's "hide extensions for known file types" is on (the
// Windows default), which would tie the label to a global setting instead of to
// the actual file name. Only ".lnk" is special-cased, because a shortcut's
// extension is an implementation detail the shell never shows either.
//
// Virtual shell items (Store apps, ::{CLSID}) have no file name to use, so they
// keep the shell name.
inline std::wstring GetFenceLabel(const std::wstring& path)
{
    if (!IsFilesystemPath(path)) return GetDisplayName(path);

    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = (slash != std::wstring::npos) ? path.substr(slash + 1) : path;

    if (name.size() > 4 &&
        _wcsicmp(name.c_str() + name.size() - 4, L".lnk") == 0)
        name.resize(name.size() - 4);

    return name.empty() ? GetDisplayName(path) : name;
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






