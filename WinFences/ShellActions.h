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

    // What the caller must still do after the menu closed.
    enum class MenuResult { Handled, Rename };

    // ---- Shell context menu (right-click) ---------------------------------
    // Shows the real Windows shell context menu for the item, plus a Rename
    // entry of our own.
    //
    // Rename cannot come from the shell here: it is a verb of the FOLDER VIEW
    // (SHELLDLL_DefView), which owns the inline edit box, not a verb of the
    // item. An IContextMenu obtained through BHID_SFUIObject has no view behind
    // it, so the shell menu genuinely contains no "rename" — verified by
    // enumerating the verbs it returns. A fence has no shell view, so the
    // editing UI is ours to provide; this function only reports the request
    // back to the caller.
    //
    // Our own commands use ids below CMD_SHELL_FIRST, the shell's start there.
    static constexpr UINT CMD_RENAME      = 1;
    static constexpr UINT CMD_SHELL_FIRST = 100;

    static MenuResult ShowContextMenu(HWND hwnd, const std::wstring& parsingName,
                                      POINT screenPt, bool allowRename = false)
    {
        if (parsingName.empty()) return MenuResult::Handled;

        // Get IShellItem for the parsing name
        ComPtr<IShellItem> pItem;
        HRESULT hr = SHCreateItemFromParsingName(
            parsingName.c_str(), nullptr, IID_PPV_ARGS(&pItem));
        if (FAILED(hr) || !pItem) return MenuResult::Handled;

        // Get IContextMenu via BHID_SFUIObject - works for all shell item types
        ComPtr<IContextMenu> pCtxMenu;
        hr = pItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&pCtxMenu));
        if (FAILED(hr) || !pCtxMenu) return MenuResult::Handled;

        // Also query IContextMenu2/3 for submenu support
        ComPtr<IContextMenu2> pCtxMenu2;
        ComPtr<IContextMenu3> pCtxMenu3;
        pCtxMenu->QueryInterface(IID_PPV_ARGS(&pCtxMenu2));
        pCtxMenu->QueryInterface(IID_PPV_ARGS(&pCtxMenu3));

        // Build the popup menu
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return MenuResult::Handled;

        hr = pCtxMenu->QueryContextMenu(hMenu, 0, CMD_SHELL_FIRST, 0x7FFF,
                                        CMF_NORMAL | CMF_EXPLORE);
        if (FAILED(hr))
        {
            DestroyMenu(hMenu);
            return MenuResult::Handled;
        }

        // Slot Rename in where Explorer has it: just above Delete.
        if (allowRename)
        {
            int deletePos = FindVerbPosition(pCtxMenu.Get(), hMenu, L"delete");
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask  = MIIM_ID | MIIM_STRING | MIIM_FTYPE;
            mii.fType  = MFT_STRING;
            mii.wID    = CMD_RENAME;
            mii.dwTypeData = const_cast<LPWSTR>(L"Rena&me");
            if (deletePos >= 0)
                InsertMenuItemW(hMenu, deletePos, TRUE, &mii);
            else
            {
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                InsertMenuItemW(hMenu, GetMenuItemCount(hMenu), TRUE, &mii);
            }
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

        // One of ours: the caller owns the follow-up.
        if (cmd == CMD_RENAME) return MenuResult::Rename;

        // Invoke the selected shell command
        if (cmd >= CMD_SHELL_FIRST)
        {
            UINT offset = cmd - CMD_SHELL_FIRST;
            CMINVOKECOMMANDINFOEX ici = { sizeof(ici) };
            ici.fMask   = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK;
            ici.hwnd    = hwnd;
            ici.lpVerb  = MAKEINTRESOURCEA(offset);
            ici.lpVerbW = MAKEINTRESOURCEW(offset);
            ici.nShow   = SW_SHOWNORMAL;
            pCtxMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&ici));
        }
        return MenuResult::Handled;
    }

    // Menu position of the item carrying `verb`, or -1. Used to place our own
    // entries where Explorer would put them.
    static int FindVerbPosition(IContextMenu* cm, HMENU menu, const wchar_t* verb)
    {
        for (int i = 0; i < GetMenuItemCount(menu); ++i)
        {
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask = MIIM_ID | MIIM_FTYPE;
            if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
            if (mii.fType & MFT_SEPARATOR) continue;
            if (mii.wID < CMD_SHELL_FIRST) continue;

            wchar_t found[64] = L"";
            if (FAILED(cm->GetCommandString(mii.wID - CMD_SHELL_FIRST, GCS_VERBW,
                    nullptr, reinterpret_cast<LPSTR>(found), 63)))
                continue;
            if (_wcsicmp(found, verb) == 0) return i;
        }
        return -1;
    }

    // ---- Rename ------------------------------------------------------------
    // Renames the item itself. For a shortcut that is the .lnk, never its
    // target. Returns the new full path, or empty on failure.
    static std::wstring RenameItem(HWND hwnd, const std::wstring& parsingName,
                                   const std::wstring& newName)
    {
        if (parsingName.empty() || newName.empty()) return {};

        ComPtr<IFileOperation> op;
        if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&op))) || !op)
            return {};

        op->SetOwnerWindow(hwnd);
        op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI);

        ComPtr<IShellItem> item;
        if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr,
                IID_PPV_ARGS(&item))) || !item)
            return {};

        if (FAILED(op->RenameItem(item.Get(), newName.c_str(), nullptr))) return {};
        if (FAILED(op->PerformOperations())) return {};

        BOOL aborted = FALSE;
        if (SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && aborted) return {};

        std::wstring folder =
            std::filesystem::path(parsingName).parent_path().wstring();
        std::wstring result = folder + L"\\" + newName;
        return (GetFileAttributesW(result.c_str()) != INVALID_FILE_ATTRIBUTES)
             ? result : std::wstring();
    }

    // Rejects names the filesystem will not take.
    static bool IsValidFileName(const std::wstring& name)
    {
        if (name.empty() || name.size() > 200) return false;
        if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
        if (name.back() == L'.' || name.back() == L' ') return false;
        return name != L"." && name != L"..";
    }

    // ---- Delete (Delete key on a selected fence icon) ----------------------
    // Deletes exactly the item at `parsingName` — for a shortcut that is the
    // .lnk itself, never its target. Always goes to the Recycle Bin so a
    // mistake stays recoverable; there is deliberately no permanent-delete path.
    static bool DeleteToRecycleBin(HWND hwnd, const std::wstring& parsingName)
    {
        if (parsingName.empty()) return false;

        ComPtr<IFileOperation> op;
        if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&op))) || !op)
            return false;

        op->SetOwnerWindow(hwnd);
        op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION
                            | FOF_NOERRORUI | FOFX_RECYCLEONDELETE);

        ComPtr<IShellItem> item;
        if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr,
                IID_PPV_ARGS(&item))) || !item)
            return false;

        if (FAILED(op->DeleteItem(item.Get(), nullptr))) return false;
        return SUCCEEDED(op->PerformOperations());
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
