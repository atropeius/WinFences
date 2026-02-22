#pragma once
// ============================================================================
// ShellActions.h — Shell integration helpers.
//
// Launch(parsingName)             — opens a file/app via ShellExecuteEx.
// ShowContextMenu(hwnd, pt, path) — shows the shell right-click context menu.
// ResolveParsingName(path)        — resolves shortcuts (.lnk) to their target.
//
// Works for regular files, .lnk shortcuts, virtual shell items, and
// Windows Store apps (via AppUserModelId).
// ============================================================================
#include "pch.h"

// ---------------------------------------------------------------------------
// ShellActions
// ---------------------------------------------------------------------------
// Handles launch (double-click) and context menu (right-click) for any shell
// item identified by its parsing name. Works for all icon types:
//   - Real files / folders / .lnk shortcuts
//   - Virtual shell items (::{CLSID})
//   - Microsoft Store / UWP apps (shell:AppsFolder\...)
//   - .url internet shortcuts
// ---------------------------------------------------------------------------

class ShellActions
{
public:
    // ---- Launch (double-click) --------------------------------------------
    // Invokes the default verb ("open") for the item.
    static void Launch(HWND hwndParent, const std::wstring& parsingName)
    {
        if (parsingName.empty()) return;

        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask  = SEE_MASK_INVOKEIDLIST | SEE_MASK_ASYNCOK;
        sei.hwnd   = hwndParent;
        sei.lpVerb = L"open";
        sei.lpFile = parsingName.c_str();
        sei.nShow  = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    // ---- Shell context menu (right-click) ---------------------------------
    // Shows the real Windows shell context menu for the item.
    // Handles submenus correctly via IContextMenu2/3 message forwarding.
    static void ShowContextMenu(HWND hwnd, const std::wstring& parsingName, POINT screenPt)
    {
        if (parsingName.empty()) return;

        // Get IShellItem for the parsing name
        ComPtr<IShellItem> pItem;
        HRESULT hr = SHCreateItemFromParsingName(
            parsingName.c_str(), nullptr, IID_PPV_ARGS(&pItem));
        if (FAILED(hr) || !pItem) return;

        // Get IContextMenu via BHID_SFUIObject - works for all shell item types
        ComPtr<IContextMenu> pCtxMenu;
        hr = pItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&pCtxMenu));
        if (FAILED(hr) || !pCtxMenu) return;

        // Also query IContextMenu2/3 for submenu support
        ComPtr<IContextMenu2> pCtxMenu2;
        ComPtr<IContextMenu3> pCtxMenu3;
        pCtxMenu->QueryInterface(IID_PPV_ARGS(&pCtxMenu2));
        pCtxMenu->QueryInterface(IID_PPV_ARGS(&pCtxMenu3));

        // Build the popup menu
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return;

        hr = pCtxMenu->QueryContextMenu(hMenu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXPLORE);
        if (FAILED(hr))
        {
            DestroyMenu(hMenu);
            return;
        }

        // Store in thread-local so the subclass proc can forward messages
        s_activeMenu2 = pCtxMenu2.Get();
        s_activeMenu3 = pCtxMenu3.Get();
        if (s_activeMenu2) s_activeMenu2->AddRef();
        if (s_activeMenu3) s_activeMenu3->AddRef();

        // Subclass the window to forward WM_MENUCHAR / WM_INITMENUPOPUP
        WNDPROC oldProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(ContextMenuSubclassProc)));

        SetForegroundWindow(hwnd);
        UINT cmd = TrackPopupMenuEx(hMenu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
            screenPt.x, screenPt.y, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0); // force menu cleanup

        // Restore original window proc
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oldProc));

        // Release IContextMenu2/3 references
        if (s_activeMenu2) { s_activeMenu2->Release(); s_activeMenu2 = nullptr; }
        if (s_activeMenu3) { s_activeMenu3->Release(); s_activeMenu3 = nullptr; }

        DestroyMenu(hMenu);

        // Invoke the selected command
        if (cmd >= 1)
        {
            CMINVOKECOMMANDINFOEX ici = { sizeof(ici) };
            ici.fMask   = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK;
            ici.hwnd    = hwnd;
            ici.lpVerb  = MAKEINTRESOURCEA(cmd - 1);
            ici.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
            ici.nShow   = SW_SHOWNORMAL;
            pCtxMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&ici));
        }
    }

    // ---- Fill FenceIconEntry from a dropped path -------------------------
    // Resolves the shell parsing name for any path that was dropped.
    static std::wstring ResolveParsingName(const std::wstring& droppedPath)
    {
        // Try to get shell parsing name - handles .lnk, virtual items, store apps
        ComPtr<IShellItem> pItem;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                droppedPath.c_str(), nullptr, IID_PPV_ARGS(&pItem))) && pItem)
        {
            PWSTR pszName = nullptr;
            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &pszName))
                && pszName)
            {
                std::wstring result(pszName);
                CoTaskMemFree(pszName);
                return result;
            }
        }
        // Fallback: use the path as-is
        return droppedPath;
    }

private:
    // Thread-local IContextMenu2/3 pointers for submenu forwarding
    static inline IContextMenu2* s_activeMenu2 = nullptr;
    static inline IContextMenu3* s_activeMenu3 = nullptr;

    static LRESULT CALLBACK ContextMenuSubclassProc(
        HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        // Forward messages required for animated submenus and owner-draw items
        if (s_activeMenu3)
        {
            LRESULT res = 0;
            if (SUCCEEDED(s_activeMenu3->HandleMenuMsg2(msg, wp, lp, &res)))
                return res;
        }
        else if (s_activeMenu2)
        {
            if (msg == WM_INITMENUPOPUP || msg == WM_MENUCHAR || msg == WM_DRAWITEM
                || msg == WM_MEASUREITEM)
            {
                s_activeMenu2->HandleMenuMsg(msg, wp, lp);
                return (msg == WM_MENUCHAR) ? MAKELRESULT(0, MNC_CLOSE) : 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};
