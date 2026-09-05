#pragma once
// ============================================================================
// FenceDropTarget.h — OLE IDropTarget for dropping files INTO a fence.
//
// Registered on each fence window. Accepts shell items (CF_HDROP and
// CFSTR_SHELLIDLIST) and puts the files into the fence's data folder.
//
// Left-drag  → files are moved (the long-standing default).
// Right-drag → on release a shell-style menu asks Copy / Move / Create
//              shortcuts / Cancel, exactly like dropping on a folder in
//              Explorer. The right button is latched during DragEnter/DragOver
//              because by the time Drop() runs the button is already up.
//
// Drop position determines insert index (closest icon slot).
// Visual drop highlight is communicated back to FenceWindow via callback.
// ============================================================================
#include "pch.h"
#include "FileOps.h"
#include "ShellActions.h"
#include "FenceData.h"
#include "DpiHelper.h"

// ---------------------------------------------------------------------------
// ExtractShellItems
// ---------------------------------------------------------------------------
// Extracts shell items from a drop data object, returning both the shell
// parsing name (works for ALL item types) and the filesystem path (if any).
// Prefers IShellItemArray (handles virtual items) then falls back to CF_HDROP.

inline std::vector<std::pair<std::wstring,std::wstring>>
    ExtractShellItems(IDataObject* pDataObj)
{
    std::vector<std::pair<std::wstring,std::wstring>> items; // (parsingName, fsPath)

    // Primary: IShellItemArray - handles virtual items, Store apps, everything
    ComPtr<IShellItemArray> pItemArray;
    if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(
            pDataObj, IID_PPV_ARGS(&pItemArray))) && pItemArray)
    {
        DWORD count = 0;
        pItemArray->GetCount(&count);
        for (DWORD i = 0; i < count; ++i)
        {
            ComPtr<IShellItem> pItem;
            if (FAILED(pItemArray->GetItemAt(i, &pItem))) continue;

            PWSTR pszParsing = nullptr;
            pItem->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &pszParsing);
            std::wstring parsing = pszParsing ? pszParsing : L"";
            CoTaskMemFree(pszParsing);

            PWSTR pszPath = nullptr;
            pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
            std::wstring path = pszPath ? pszPath : L"";
            CoTaskMemFree(pszPath);

            if (!parsing.empty())
                items.push_back({parsing, path});
        }
        if (!items.empty()) return items;
    }

    // Fallback: CF_HDROP plain file paths
    FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg = {};
    if (SUCCEEDED(pDataObj->GetData(&fe, &stg)))
    {
        HDROP hDrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
        if (hDrop)
        {
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i)
            {
                UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring path(len + 1, L'\0');
                DragQueryFileW(hDrop, i, path.data(), len + 1);
                path.resize(len);
                std::wstring parsing = ShellActions::ResolveParsingName(path);
                items.push_back({parsing, path});
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
    }

    return items;
}

// ---------------------------------------------------------------------------
// FenceDropTarget
// ---------------------------------------------------------------------------

class FenceDropTarget : public IDropTarget
{
public:
    // items = vector of (parsingName, fsPath) pairs
    using OnDropCallback =
        std::function<void(const std::vector<std::pair<std::wstring,std::wstring>>&,
                           DropAction)>;

    FenceDropTarget(HWND hwnd, const std::wstring& fenceId, OnDropCallback cb)
        : m_hwnd(hwnd), m_fenceId(fenceId), m_onDrop(std::move(cb)), m_refCount(1)
    {}

    // ---- IUnknown ----
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // ---- IDropTarget ----
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj,
        DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        m_hasFiles  = CanAccept(pDataObj);
        m_rightDrag = (grfKeyState & MK_RBUTTON) != 0;
        // Advertise COPY only - Explorer won't attempt its own move/delete,
        // so no "same name" conflict dialog. We handle the physical move ourselves.
        *pdwEffect = m_hasFiles ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (m_hasFiles) PostMessageW(m_hwnd, WM_APP + 50, pt.x, pt.y);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState,
        POINTL pt, DWORD* pdwEffect) override
    {
        // Latch, never clear: Drop() sees the button already released, and some
        // sources drop a frame with no button bits set right before the drop.
        if (grfKeyState & MK_RBUTTON) m_rightDrag = true;
        *pdwEffect = m_hasFiles ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (m_hasFiles) PostMessageW(m_hwnd, WM_APP + 50, pt.x, pt.y);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        m_hasFiles  = false;
        m_rightDrag = false;
        PostMessageW(m_hwnd, WM_APP + 51, 0, 0);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj,
        DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        PostMessageW(m_hwnd, WM_APP + 51, 0, 0); // hide highlight

        bool rightDrag = m_rightDrag || (grfKeyState & MK_RBUTTON) != 0;
        m_hasFiles  = false;
        m_rightDrag = false;

        auto items = ExtractShellItems(pDataObj);
        if (items.empty()) { *pdwEffect = DROPEFFECT_NONE; return S_OK; }

        // Right-drag: ask what to do, exactly like dropping on a folder.
        // Blocking the source inside Drop() while the menu is up is what
        // Explorer does as well.
        DropAction action = DropAction::Move;
        if (rightDrag && !AskDropAction(action))
        {
            *pdwEffect = DROPEFFECT_NONE; // user picked Cancel
            return S_OK;
        }

        // Pre-rename any movable file whose name conflicts in the fence data folder.
        // We do this NOW - synchronously inside Drop() - before Explorer has a chance
        // to do anything with the file. The rename happens at the source location
        // (e.g. Desktop), so "Dokument.txt" becomes "Dokument (1).txt" on the Desktop
        // before we move it. Explorer sees the rename via SHChangeNotify and is happy.
        // We also check (1), (2), (3)... until we find a free name.
        //
        // Move only: copy and shortcut must leave the source untouched, so they
        // resolve the collision at the destination instead (FileOps::MakeFreeDestPath).
        if (action == DropAction::Move && !m_fenceId.empty())
        {
            std::wstring dataFolder = FileOps::GetFenceDataFolder(m_fenceId);
            if (!dataFolder.empty())
            {
                for (auto& [parsingName, fsPath] : items)
                {
                    if (!FileOps::IsMovable(parsingName)) continue;

                    // Skip if file is already in our data folder (dropped back on own fence)
                    std::wstring srcFolder = std::filesystem::path(parsingName)
                                                .parent_path().wstring();
                    if (_wcsicmp(srcFolder.c_str(), dataFolder.c_str()) == 0)
                        continue;

                    std::wstring renamed = FileOps::PreRenameForDest(parsingName, dataFolder);
                    if (renamed != parsingName)
                    {
                        fsPath      = renamed;
                        parsingName = renamed;
                    }
                }
            }
        }

        // Always NONE: we perform the physical operation ourselves, so the
        // source must not additionally delete the original on a MOVE effect.
        *pdwEffect = DROPEFFECT_NONE;
        // Log what we're about to drop
        for (auto& [pn, fp] : items)
        {
            wchar_t dbg[512];
            swprintf_s(dbg, L"[WinFences] Drop(%s): parsingName=%s fsPath=%s",
                ActionName(action), pn.c_str(), fp.c_str());
            OutputDebugStringW(dbg);
        }
        if (m_onDrop) m_onDrop(items, action);
        return S_OK;
    }

private:
    HWND           m_hwnd;
    std::wstring   m_fenceId;
    OnDropCallback m_onDrop;
    LONG           m_refCount;
    bool           m_hasFiles  = false;
    bool           m_rightDrag = false;

    static const wchar_t* ActionName(DropAction a)
    {
        switch (a)
        {
        case DropAction::Copy:     return L"copy";
        case DropAction::Shortcut: return L"shortcut";
        default:                   return L"move";
        }
    }

    // Shell-style right-drag menu. Returns false if the user cancelled
    // (explicit Cancel, Escape, or a click outside the menu).
    bool AskDropAction(DropAction& out) const
    {
        enum { CMD_COPY = 1, CMD_MOVE, CMD_SHORTCUT, CMD_CANCEL };

        HMENU menu = CreatePopupMenu();
        if (!menu) return false;
        AppendMenuW(menu, MF_STRING, CMD_COPY,     L"&Copy here");
        AppendMenuW(menu, MF_STRING, CMD_MOVE,     L"&Move here");
        AppendMenuW(menu, MF_STRING, CMD_SHORTCUT, L"Create &shortcuts here");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CMD_CANCEL,   L"Cancel");
        // Move stays the fence default, matching a plain left-drag.
        SetMenuDefaultItem(menu, CMD_MOVE, FALSE);

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(m_hwnd);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(menu);

        switch (cmd)
        {
        case CMD_COPY:     out = DropAction::Copy;     return true;
        case CMD_MOVE:     out = DropAction::Move;     return true;
        case CMD_SHORTCUT: out = DropAction::Shortcut; return true;
        default:           return false; // CMD_CANCEL or dismissed
        }
    }

    bool CanAccept(IDataObject* pDataObj) const
    {
        FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        if (pDataObj->QueryGetData(&fe) == S_OK) return true;
        CLIPFORMAT cf = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
        fe.cfFormat = cf; fe.tymed = TYMED_HGLOBAL;
        return pDataObj->QueryGetData(&fe) == S_OK;
    }
};
