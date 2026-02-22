#pragma once
// ============================================================================
// FenceWindow.h — One fence: a Win32 window that displays a grid of icons.
//
// Each fence is a layered WS_POPUP window with DWM glass background.
// All layout is driven by FenceData.col/row/cols/rows via GridSystem.h.
//
// Key responsibilities:
//   Rendering  — Direct2D: background, header bar, icon bitmaps, labels,
//                selection highlight, drop highlight, expanded label popup
//   Input      — drag-to-move (title bar), resize (edges/corners via
//                WM_NCHITTEST), icon click/double-click/right-click
//   Snap       — WM_EXITSIZEMOVE converts physical pixel position back to
//                col/row (PixelToCol/Row), then re-derives canonical pixels
//                (FenceWindowX/Y/W/H) — grid is always the truth
//   OLE D&D    — FenceDropTarget (drop in), FenceDragSource (drag out)
//   File sync  — FileWatcher triggers re-scan of data folder
//
// Coordinate rule: D2D render target has dpiX=dpiY=96 ? 1 unit = 1 physical
//                  pixel. All D2D coordinates (CellX/Y, IconRect, LabelRect,
//                  font sizes) are therefore in PHYSICAL pixels.
// ============================================================================
#include "pch.h"
#include "FileWatcher.h"
#include "DpiHelper.h"
#include "Renderer.h"
#include "IconCache.h"
#include "FenceData.h"
#include "GridSystem.h"
#include "FenceDropTarget.h"
#include "ShellActions.h"
#include "FileOps.h"
#include "FenceDragSource.h"

class FenceWindow
{
public:
    static constexpr wchar_t CLASS_NAME[] = L"WinFences_Fence";

    static void RegisterClass(HINSTANCE hInst)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc   = FenceWindow::StaticWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = CLASS_NAME;
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);
    }

    FenceWindow(HINSTANCE hInst, IconCache* cache)
        : m_hInst(hInst), m_iconCache(cache) {}

    ~FenceWindow()
    {
        if (m_hwnd)
        {
            RevokeDragDrop(m_hwnd);
            DestroyWindow(m_hwnd);
        }
        if (m_dropTarget) { m_dropTarget->Release(); m_dropTarget = nullptr; }
    }

    bool Create(const FenceData& data, HWND /*unused*/)
    {
        m_data = data;
        SortIconsByName(m_data.icons);

        // data.x/y are 96-DPI logical coordinates.
        // MonitorFromPoint needs PHYSICAL screen coordinates.
        // Bootstrap: assume primary monitor scale to find the monitor,
        // then refine with the actual monitor's scale.
        HMONITOR hmonPrimary = MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY);
        float scalePrimary   = DpiHelper::ScaleForMonitor(hmonPrimary);

        POINT physPt = { static_cast<LONG>(GridToPixelX(data.col, scalePrimary)),
                     static_cast<LONG>(GridToPixelY(data.row, scalePrimary)) };
        HMONITOR hmon = MonitorFromPoint(physPt, MONITOR_DEFAULTTOPRIMARY);
        float scale   = DpiHelper::ScaleForMonitor(hmon);

        // Now compute correct physical coords with the real monitor scale
        int physX = FenceWindowX(data.col,  scale);
        int physY = FenceWindowY(data.row,  scale);
        int physW = FenceWindowW(data.cols, scale);
        int physH = FenceWindowH(data.rows, scale);

        DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;

        m_hwnd = CreateWindowExW(exStyle, CLASS_NAME, nullptr,
            WS_POPUP,
            physX, physY, physW, physH,
            nullptr, nullptr, m_hInst, this);
        if (!m_hwnd) return false;

        // Z-order bottom BEFORE showing — prevents Windows from raising us on first paint
        SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);

        // Anchor to Progman (the desktop window) so Win+D / Show Desktop
        // does not hide fences.
		if (HWND hProgman = FindWindowExW(nullptr, nullptr, L"Progman", nullptr))
			SetWindowLongPtrW(m_hwnd, GWLP_HWNDPARENT,
				reinterpret_cast<LONG_PTR>(hProgman));

		// Strip minimize/maximize so Win+D keyboard shortcut can't minimize
		LONG_PTR wstyle = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
		SetWindowLongPtrW(m_hwnd, GWL_STYLE,
			wstyle & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX);

        // Place at Z-order bottom immediately — WM_WINDOWPOSCHANGING then
        // prevents anyone from raising us. (NoFences technique)
        SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(m_hwnd, &margins);

        HRESULT hr = m_renderer.Initialize(m_hwnd);
        if (FAILED(hr)) { DebugLog(L"Renderer init failed: 0x%08X", hr); return false; }

        // Snap to grid on creation — size from col/row counts, position from grid
        {
            float sc = DpiHelper::ScaleForWindow(m_hwnd);
            SetWindowPos(m_hwnd, nullptr,
                FenceWindowX(data.col,  sc),
                FenceWindowY(data.row,  sc),
                FenceWindowW(data.cols, sc),
                FenceWindowH(data.rows, sc),
                SWP_NOZORDER | SWP_NOACTIVATE);
            m_renderer.Resize();
        }

        CreateRenderResources();
        Render();

        // Shell change notification + FileWatcher for data folder
        // FileWatcher catches Cut/Delete/Rename via ReadDirectoryChangesW
        RegisterShellNotify();
        m_fileWatcher.Start(m_hwnd, FileOps::GetFenceDataFolder(m_data.id));

        // OLE Drag & Drop
        m_dropTarget = new FenceDropTarget(m_hwnd, m_data.id,
            [this](const std::vector<std::pair<std::wstring,std::wstring>>& items)
            { OnItemsDropped(items); });
        RegisterDragDrop(m_hwnd, m_dropTarget);

        // Post a deferred render so the icon size COM query runs after the
        // message pump is active (it may fail during synchronous window creation).
        PostMessageW(m_hwnd, WM_APP + 52, 0, 0);

        return true;
    }

    void SetData(const FenceData& data)
    {
        m_data = data;
        SortIconsByName(m_data.icons);
        if (m_hwnd)
        {
            float scale = DpiHelper::ScaleForWindow(m_hwnd);
            SetWindowPos(m_hwnd, nullptr,
                FenceWindowX(data.col,  scale),
                FenceWindowY(data.row,  scale),
                FenceWindowW(data.cols, scale),
                FenceWindowH(data.rows, scale),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Render();
    }

    bool IsSizingResolved() const { return m_wasSizing; }

    FenceData GetData() const
    {
        // col/row/cols/rows in m_data ARE the truth — always kept up to date.
        // No back-calculation from pixels needed.
        return m_data;
    }

    HWND      GetHwnd()          const { return m_hwnd; }
    Renderer& GetRenderer_Hack()       { return m_renderer; }
    void      SetMessageWindow(HWND w) { m_msgWnd_external = w; }

    void Render()
    {
        if (!m_hwnd || !m_renderer.IsReady()) return;

        RECT clientRc; GetClientRect(m_hwnd, &clientRc);
        float scale = DpiHelper::ScaleForWindow(m_hwnd);
        float W     = static_cast<float>(clientRc.right);   // physical pixels (D2D dpi=96)
        float H     = static_cast<float>(clientRc.bottom);

        int g_cols = m_data.cols;
        int g_rows = m_data.rows;

        DebugLog(L"[Render] scale=%.2f physW=%.0f physH=%.0f cellPhys=%d iconPhys=%d",
            scale, W, H,
            static_cast<int>(Grid_CellPx(scale)*scale), static_cast<int>(Grid_IconPx(scale)*scale));

        m_renderer.BeginDraw();
        auto dc = m_renderer.DC();

        // ---- Background ----
        D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), 10.0f, 10.0f);
        if (m_bgBrush)    { m_bgBrush->SetOpacity(m_data.alpha); dc->FillRoundedRectangle(bgRect, m_bgBrush.Get()); }
        if (m_borderBrush) dc->DrawRoundedRectangle(bgRect, m_borderBrush.Get(), 1.0f);

        // ---- Label bar ----
        // Clip to the background rounded rect, then fill the label strip.
        // This preserves both top corners correctly.
        float lh = static_cast<float>(Grid_HeaderPx() * scale);  // physical
        if (m_labelBgBrush)
        {
            // Push a rounded-rect clip matching the background shape
            D2D1_ROUNDED_RECT clipRR = D2D1::RoundedRect(
                D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), 10.0f, 10.0f);
            ComPtr<ID2D1RoundedRectangleGeometry> clipGeom;
            m_renderer.Factory()->CreateRoundedRectangleGeometry(clipRR, &clipGeom);
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),
                clipGeom.Get()), nullptr);

            // Fill label strip (flat rect - corners handled by clip layer)
            dc->FillRectangle(D2D1::RectF(0.0f, 0.0f, W, lh), m_labelBgBrush.Get());

            dc->PopLayer();
        }

        // Label text
        if (m_textFormat && m_textBrush && !m_data.label.empty())
        {
            float pad = static_cast<float>(Grid_MarginPx() * scale);  // physical
            dc->DrawText(m_data.label.c_str(),
                static_cast<UINT32>(m_data.label.size()), m_textFormat.Get(),
                D2D1::RectF(pad, 0, W - pad, lh), m_textBrush.Get());
        }

        // ---- Icons + truncated labels ----
        // labelH = Grid_LabelPx: space reserved below icon for label text
        //ready reserved between rows.
        float labelH   = m_iconLabelFormat ? static_cast<float>(Grid_LabelPx(scale) * scale) : 0.0f;  // physical
        float labelPad = 3.0f; // horizontal padding inside label rect

        for (int i = 0; i < static_cast<int>(m_data.icons.size()); ++i)
        {
            int col = i % g_cols;
            int row = i / g_cols;
            if (row >= g_rows) break;

            D2D1_RECT_F dest = IconRect(col, row, scale);

            // Drop highlight (behind icon)
            if (m_dropHighlight && m_dropHighlightIndex == i)
            {
                ComPtr<ID2D1SolidColorBrush> hl, hlb;
                dc->CreateSolidColorBrush(D2D1::ColorF(0.4f,0.7f,1.0f,0.35f), &hl);
                dc->CreateSolidColorBrush(D2D1::ColorF(0.5f,0.8f,1.0f,0.90f), &hlb);
                D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(dest, 4.0f, 4.0f);
                dc->FillRoundedRectangle(rr, hl.Get());
                dc->DrawRoundedRectangle(rr, hlb.Get(), 1.5f);
            }

            // Icon bitmap
            if (m_iconCache)
            {
                // Use parsingName as cache key (= current filesystem path after moves)
                const std::wstring& iconKey = m_data.icons[i].parsingName.empty()
                    ? m_data.icons[i].path
                    : m_data.icons[i].parsingName;
                m_iconCache->DrawIcon(dc, iconKey, dest);
            }

            // Selection highlight border (on top of icon)
            if (m_selectedIndex == i)
            {
                D2D1_ROUNDED_RECT selRR = D2D1::RoundedRect(
                    D2D1::RectF(dest.left-3.0f, dest.top-3.0f,
                                dest.right+3.0f, dest.bottom+3.0f), 5.0f, 5.0f);
                ComPtr<ID2D1SolidColorBrush> selFill, selBorder;
                dc->CreateSolidColorBrush(D2D1::ColorF(1.0f,1.0f,1.0f,0.18f), &selFill);
                dc->CreateSolidColorBrush(D2D1::ColorF(1.0f,1.0f,1.0f,0.85f), &selBorder);
                dc->FillRoundedRectangle(selRR, selFill.Get());
                dc->DrawRoundedRectangle(selRR, selBorder.Get(), 1.5f);
            }

            // Truncated label below icon (always shown, single line with ellipsis)
            if (m_iconLabelFormat && labelH > 0.0f && !m_data.icons[i].displayName.empty())
            {
                // Place label in the full iconLabelH slot below the icon.
                // Top gap = 2px, bottom uses remaining space.
                D2D1_RECT_F lblRect = LabelRect(col, row, scale);  // full iconLabelH space below icon
                dc->DrawText(
                    m_data.icons[i].displayName.c_str(),
                    static_cast<UINT32>(m_data.icons[i].displayName.size()),
                    m_iconLabelFormat.Get(), lblRect, m_textBrush.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }

        // Drop highlight on empty slot
        if (m_dropHighlight && m_dropHighlightIndex == static_cast<int>(m_data.icons.size()))
        {
            int col = m_dropHighlightIndex % m_data.cols;
            int row = m_dropHighlightIndex / m_data.cols;
            if (row < m_data.rows)
            {
                D2D1_RECT_F dest = IconRect(col, row, scale);
                ComPtr<ID2D1SolidColorBrush> hl, hlb;
                dc->CreateSolidColorBrush(D2D1::ColorF(0.4f,0.7f,1.0f,0.35f), &hl);
                dc->CreateSolidColorBrush(D2D1::ColorF(0.5f,0.8f,1.0f,0.90f), &hlb);
                D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(dest, 4.0f, 4.0f);
                dc->FillRoundedRectangle(rr, hl.Get());
                dc->DrawRoundedRectangle(rr, hlb.Get(), 1.5f);
            }
        }

        // ---- Expanded label popup for selected icon (drawn last = on top) ----
        if (m_selectedIndex >= 0
            && m_selectedIndex < static_cast<int>(m_data.icons.size())
            && m_iconLabelFullFormat && m_iconLabelBgBrush && m_textBrush)
        {
            const auto& selIcon = m_data.icons[m_selectedIndex];
            if (!selIcon.displayName.empty())
            {
                int selCol, selRow;
                selCol = m_selectedIndex % m_data.cols;
                selRow = m_selectedIndex / m_data.cols;
                D2D1_RECT_F iconDest = IconRect(selCol, selRow, scale);

                // Measure the full text width using a DWrite layout
                const std::wstring& fullName = selIcon.displayName;
                ComPtr<IDWriteTextLayout> layout;
                m_renderer.DWrite()->CreateTextLayout(
                    fullName.c_str(),
                    static_cast<UINT32>(fullName.size()),
                    m_iconLabelFullFormat.Get(),
                    10000.0f, labelH,  // unconstrained width, single line height
                    &layout);

                float textW = static_cast<float>(Grid_IconPx(scale) * scale); // physical
                float textH = labelH;
                if (layout)
                {
                    DWRITE_TEXT_METRICS tm = {};
                    layout->GetMetrics(&tm);
                    textW = tm.width  + 1.0f;
                    textH = tm.height + 1.0f;
                }

                const float hPad = 6.0f;  // horizontal padding inside popup
                const float vPad = 4.0f;  // vertical padding inside popup
                float popW = textW + hPad * 2.0f;
                float popH = textH + vPad * 2.0f;

                // Clamp width to fence width
                popW = fminf(popW, W - 8.0f);

                // Center popup horizontally over icon center
                float iconCX = (iconDest.left + iconDest.right) * 0.5f;
                float popL = iconCX - popW * 0.5f;
                float popR = popL + popW;
                // Keep inside fence
                if (popL < 4.0f) { popL = 4.0f; popR = popL + popW; }
                if (popR > W - 4.0f) { popR = W - 4.0f; popL = popR - popW; }

                // Place below icon; if no room, place above
                float popT = iconDest.bottom + 2.0f;
                float popB = popT + popH;
                if (popB > H - 4.0f)
                {
                    popB = iconDest.top - 2.0f;
                    popT = popB - popH;
                }

                D2D1_ROUNDED_RECT popRR = D2D1::RoundedRect(
                    D2D1::RectF(popL, popT, popR, popB), 4.0f, 4.0f);

                // Draw opaque background + border
                dc->FillRoundedRectangle(popRR, m_iconLabelBgBrush.Get());
                ComPtr<ID2D1SolidColorBrush> popBorder;
                dc->CreateSolidColorBrush(D2D1::ColorF(0.55f,0.62f,0.88f,0.80f), &popBorder);
                dc->DrawRoundedRectangle(popRR, popBorder.Get(), 1.0f);

                // Draw full text centered inside popup
                D2D1_RECT_F textRect = D2D1::RectF(
                    popL + hPad, popT + vPad,
                    popR - hPad, popB - vPad);
                dc->DrawText(
                    fullName.c_str(),
                    static_cast<UINT32>(fullName.size()),
                    m_iconLabelFullFormat.Get(),
                    textRect, m_textBrush.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        m_renderer.EndDraw();
    }


    // Sync data folder -> GUI: add icons for files on disk not yet in m_data.icons
    // and remove icons whose files are gone. Called on startup and after restore.
    void SyncDataFolder()
    {
        std::wstring dataFolder = FileOps::GetFenceDataFolder(m_data.id);
        if (dataFolder.empty() || !std::filesystem::exists(dataFolder)) return;

        // 1. Add orphan files (on disk but not in GUI)
        bool changed = false;
        for (auto& entry : std::filesystem::directory_iterator(dataFolder))
        {
            if (!entry.is_regular_file()) continue;
            std::wstring path = entry.path().wstring();

            bool found = false;
            for (auto& icon : m_data.icons)
                if (_wcsicmp(icon.parsingName.c_str(), path.c_str()) == 0)
                    { found = true; break; }
            if (found) continue;

            FenceIconEntry e;
            e.path        = path;
            e.parsingName = path;
            e.displayName = entry.path().filename().wstring();
            m_data.icons.push_back(std::move(e));
            DebugLog(L"[Sync] added: %s", path.c_str());
            changed = true;
        }

        // 2. Remove icons whose files are gone (forward to PruneDeletedIcons)
        bool pruned = PruneDeletedIcons();
        changed = changed || pruned;

        if (changed)
        {
            Render();
            if (m_msgWnd_external)
                PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);
        }
    }

    // Delete files in data folder not referenced by any icon (leftover orphans).
    // Called when a fence is deleted.
    void CleanOrphanFiles()
    {
        std::wstring dataFolder = FileOps::GetFenceDataFolder(m_data.id);
        if (dataFolder.empty() || !std::filesystem::exists(dataFolder)) return;

        for (auto& entry : std::filesystem::directory_iterator(dataFolder))
        {
            if (!entry.is_regular_file()) continue;
            std::wstring path = entry.path().wstring();

            bool found = false;
            for (auto& icon : m_data.icons)
                if (_wcsicmp(icon.parsingName.c_str(), path.c_str()) == 0)
                    { found = true; break; }

            if (!found)
            {
                DebugLog(L"[Sync] deleting orphan: %s", path.c_str());
                std::filesystem::remove(entry.path());
            }
        }
    }


private:
    HINSTANCE  m_hInst           = nullptr;
    HWND       m_hwnd            = nullptr;
    HWND       m_msgWnd_external = nullptr;
    FenceData  m_data;
    Renderer   m_renderer;
    IconCache* m_iconCache       = nullptr;

    ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    ComPtr<ID2D1SolidColorBrush> m_labelBgBrush;
    ComPtr<ID2D1SolidColorBrush> m_textBrush;
    ComPtr<IDWriteTextFormat>     m_textFormat;
    // Icon label resources
    ComPtr<IDWriteTextFormat>     m_iconLabelFormat;     // truncated label under icon
    ComPtr<IDWriteTextFormat>     m_iconLabelFullFormat; // full-name expanded label
    ComPtr<ID2D1SolidColorBrush> m_iconLabelBgBrush;    // opaque bg for expanded label

    // (move/resize handled natively via HTCAPTION/HTBOTTOMRIGHT)

    // Drop state
    FenceDropTarget* m_dropTarget        = nullptr;
    bool             m_dropHighlight     = false;
    bool             m_isSizing          = false;
    bool             m_wasSizing         = false;
    int              m_dropHighlightIndex = -1;
    int              m_selectedIndex      = -1;  // icon selection highlight

    // Global mouse hook - active only while an icon is selected
    // Detects clicks outside this window so we can deselect
    HHOOK            m_mouseHook          = nullptr;

    // Shell change notification cookie
    ULONG m_shellNotifyCookie = 0;

    // FileSystem watcher for data folder (catches Cut, Delete, Rename)
    FileWatcher m_fileWatcher;

    // Pending file moves (deferred until after Explorer releases file lock)
    std::vector<std::wstring> m_pendingMoves;

    // Drag-out state
    bool             m_dragPending        = false; // mouse down on icon, drag not yet started
    int              m_dragIndex          = -1;    // icon being dragged
    POINT            m_dragStartPt        = {};    // screen coords where drag started

    // ---- Drag out of fence ----

    void BeginDragOut(int iconIndex)
    {
        if (iconIndex < 0 || iconIndex >= static_cast<int>(m_data.icons.size()))
            return;

        // Copy everything we need BEFORE DoDragDrop (which blocks and could
        // invalidate references into m_data.icons)
        const std::wstring parsingName = m_data.icons[iconIndex].parsingName;
        const bool movable = FileOps::IsMovable(parsingName);

        // Pre-rename the file in the data folder if the same name already
        // exists on the Desktop. This must happen BEFORE DoDragDrop so the
        // IDataObject we pass to the Desktop drop target has the correct name.
        std::wstring actualParsingName = parsingName;
        if (movable)
        {
            std::wstring desktop = FileOps::GetDesktopPath();
            if (!desktop.empty())
            {
                std::wstring renamed = FileOps::PreRenameForDest(parsingName, desktop);
                if (renamed != parsingName)
                {
                    DebugLog(L"[DragOut] pre-renamed: %s -> %s",
                        parsingName.c_str(), renamed.c_str());
                    // Update the icon entry so parsingName stays in sync
                    for (auto& e : m_data.icons)
                        if (_wcsicmp(e.parsingName.c_str(), parsingName.c_str()) == 0)
                            { e.parsingName = renamed; e.path = renamed; break; }
                    actualParsingName = renamed;
                }
            }
        }

        // Build IDataObject with the (possibly renamed) path
        ComPtr<IDataObject> pDataObj;
        if (FAILED(CreateFileDataObject(actualParsingName, &pDataObj)) || !pDataObj)
            return;

        ComPtr<FenceDragSource> pSource = new FenceDragSource();

        m_selectedIndex = -1;
        UninstallMouseHook();
        Render();

        // Blocks until user drops or presses Escape
        DWORD dwEffect = DROPEFFECT_MOVE | DROPEFFECT_COPY | DROPEFFECT_LINK;
        DWORD dwResultEffect = DROPEFFECT_NONE;
        HRESULT hr = DoDragDrop(pDataObj.Get(), pSource.Get(),
                                dwEffect, &dwResultEffect);

        DebugLog(L"[DragOut] hr=%08x effect=%lu path=%s",
            hr, dwResultEffect, actualParsingName.c_str());

        // DRAGDROP_S_DROP + DROPEFFECT_NONE = dropped back on own fence or rejected
        // In that case do nothing - icon stays in fence
        if (hr != DRAGDROP_S_DROP) return; // cancelled (Escape)
        if (dwResultEffect == DROPEFFECT_NONE)
        {
            DebugLog(L"[DragOut] dropped with NONE effect - staying in fence");
            return;
        }

        // Find icon by parsingName (use updated name)
        int idx = -1;
        for (int i = 0; i < static_cast<int>(m_data.icons.size()); ++i)
            if (_wcsicmp(m_data.icons[i].parsingName.c_str(), actualParsingName.c_str()) == 0)
                { idx = i; break; }
        if (idx < 0) return;

        POINT cursorPt = {};
        GetCursorPos(&cursorPt);
        bool droppedOnDesktop = IsDesktopWindow(WindowFromPoint(cursorPt));

        if (movable && droppedOnDesktop)
        {
            std::wstring newPath = FileOps::MoveToDesktop(actualParsingName);
            DebugLog(L"[DragOut] MoveToDesktop -> %s", newPath.c_str());
            if (m_iconCache) m_iconCache->Invalidate(actualParsingName);
        }
        else if (!movable)
        {
            // Virtual item - nothing to move
        }
        else
        {
            if (m_iconCache) m_iconCache->Invalidate(actualParsingName);
        }

        RemoveIconAt(idx);
    }

    // Check if hwnd belongs to the desktop shell
    static bool IsDesktopWindow(HWND hwnd)
    {
        if (!hwnd) return false;
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        // Desktop icon area: SysListView32 inside SHELLDLL_DefView inside WorkerW/Progman
        if (_wcsicmp(cls, L"SysListView32") == 0) return true;
        if (_wcsicmp(cls, L"SHELLDLL_DefView") == 0) return true;
        if (_wcsicmp(cls, L"Progman") == 0) return true;
        if (_wcsicmp(cls, L"WorkerW") == 0) return true;
        return false;
    }

    void RegisterShellNotify()
    {
        if (m_shellNotifyCookie != 0) return;
        std::wstring dataFolder = FileOps::GetFenceDataFolder(m_data.id);
        if (dataFolder.empty()) return;

        SHChangeNotifyEntry entry = {};
        ComPtr<IShellItem> pItem;
        if (FAILED(SHCreateItemFromParsingName(dataFolder.c_str(),
                nullptr, IID_PPV_ARGS(&pItem)))) return;

        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHGetIDListFromObject(pItem.Get(), &pidl))) return;

        entry.pidl      = pidl;
        entry.fRecursive = FALSE;

        m_shellNotifyCookie = SHChangeNotifyRegister(
            m_hwnd,
            SHCNRF_ShellLevel | SHCNRF_NewDelivery,
            SHCNE_DELETE | SHCNE_RENAMEITEM | SHCNE_RMDIR,
            WM_APP + 58,
            1, &entry);

        ILFree(pidl);
        DebugLog(L"ShellNotify registered, cookie=%lu", m_shellNotifyCookie);
    }

    void UnregisterShellNotify()
    {
        if (m_shellNotifyCookie)
        {
            SHChangeNotifyDeregister(m_shellNotifyCookie);
            m_shellNotifyCookie = 0;
        }
    }

    void RemoveIconAt(int idx)
    {
        if (idx < 0 || idx >= static_cast<int>(m_data.icons.size())) return;
        m_data.icons.erase(m_data.icons.begin() + idx);
        Render();
        if (m_msgWnd_external)
            PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);
    }

    // Remove icons whose physical files no longer exist on disk.
    // Virtual items (::CLSID, shell:) are always kept.
    // Returns true if any icons were removed.
    bool PruneDeletedIcons()
    {
        bool changed = false;
        for (int i = static_cast<int>(m_data.icons.size()) - 1; i >= 0; --i)
        {
            const auto& e = m_data.icons[i];
            if (!FileOps::IsMovable(e.parsingName)) continue;
            if (GetFileAttributesW(e.parsingName.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                if (m_iconCache) m_iconCache->Invalidate(e.parsingName);
                m_data.icons.erase(m_data.icons.begin() + i);
                changed = true;
            }
        }
        if (changed)
        {
            Render();
            if (m_msgWnd_external)
                PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);
        }
        return changed;
    }

    // ---- Selection hook ----

    void InstallMouseHook()
    {
        if (m_mouseHook) return;
        // Store this pointer in thread-local so the static proc can access it
        s_hookTarget = this;
        m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
            GetModuleHandleW(nullptr), 0);
    }

    void UninstallMouseHook()
    {
        if (!m_mouseHook) return;
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
        s_hookTarget = nullptr;
    }

    static inline FenceWindow* s_hookTarget = nullptr;

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wp, LPARAM lp)
    {
        if (nCode == HC_ACTION && s_hookTarget)
        {
            auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);

            // ---- Drag tracking (only when drag is armed) ----
            if (s_hookTarget->m_dragPending)
            {
                if (wp == WM_MOUSEMOVE)
                {
                    int dx = abs(info->pt.x - s_hookTarget->m_dragStartPt.x);
                    int dy = abs(info->pt.y - s_hookTarget->m_dragStartPt.y);
                    if (dx >= GetSystemMetrics(SM_CXDRAG)
                        || dy >= GetSystemMetrics(SM_CYDRAG))
                    {
                        int dragIdx = s_hookTarget->m_dragIndex;
                        s_hookTarget->m_dragPending = false;
                        s_hookTarget->m_dragIndex   = -1;
                        PostMessageW(s_hookTarget->m_hwnd, WM_APP + 54,
                            static_cast<WPARAM>(dragIdx), 0);
                        // Hook stays installed for deselect detection
                    }
                }
                else if (wp == WM_LBUTTONUP)
                {
                    s_hookTarget->m_dragPending = false;
                    s_hookTarget->m_dragIndex   = -1;
                    // Hook stays for deselect detection
                }
                // Only handle move+up during drag - pass everything else through fast
                if (wp == WM_MOUSEMOVE || wp == WM_LBUTTONUP)
                    return CallNextHookEx(nullptr, nCode, wp, lp);
            }

            // ---- Deselect on outside click ----
            // Only intercept actual button-down events - ignore moves entirely
            if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_NCLBUTTONDOWN)
            {
                HWND hwndUnder = WindowFromPoint(info->pt);
                if (hwndUnder != s_hookTarget->m_hwnd)
                    PostMessageW(s_hookTarget->m_hwnd, WM_APP + 53, 0, 0);
            }
        }
        return CallNextHookEx(nullptr, nCode, wp, lp);
    }

    // ---- Render resources ----

    void CreateRenderResources()
    {
        if (!m_renderer.IsReady()) return;
        auto dc = m_renderer.DC();
        if (!dc) return;

        // Release all D2D bitmaps bound to the OLD render target before
        // creating new resources. This prevents the multi-fence factory mismatch crash.
        if (m_iconCache) m_iconCache->InvalidateRenderTarget(dc);

        m_bgBrush.Reset(); m_borderBrush.Reset();
        m_labelBgBrush.Reset(); m_textBrush.Reset(); m_textFormat.Reset();
        m_iconLabelFormat.Reset(); m_iconLabelFullFormat.Reset(); m_iconLabelBgBrush.Reset();

        float scale = DpiHelper::ScaleForWindow(m_hwnd);

        dc->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.18f, 0.28f, m_data.alpha), &m_bgBrush);
        dc->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.62f, 0.88f, 0.85f),        &m_borderBrush);
        dc->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.22f, 0.45f, 0.92f),        &m_labelBgBrush);
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f,  1.0f,  1.0f,  0.95f),        &m_textBrush);

        float fontPx = 13.0f * scale;  // physical pixels (D2D dpi=96)
        m_renderer.DWrite()->CreateTextFormat(
            L"Segoe UI Variable", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontPx, L"", &m_textFormat);
        if (m_textFormat)
        {
            m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Icon label: scales with icon size (iconPx / 8, clamped 10..16px)
        float lblPx  = static_cast<float>(Grid_LabelPx(scale) * scale);  // physical pixels
        m_renderer.DWrite()->CreateTextFormat(
            L"Segoe UI Variable", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            lblPx, L"", &m_iconLabelFormat);
        if (m_iconLabelFormat)
        {
            m_iconLabelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_iconLabelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            m_iconLabelFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            // Enable trimming with ellipsis
            DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            ComPtr<IDWriteInlineObject> ellipsis;
            m_renderer.DWrite()->CreateEllipsisTrimmingSign(m_iconLabelFormat.Get(), &ellipsis);
            m_iconLabelFormat->SetTrimming(&trim, ellipsis.Get());
        }

        // Full-name format: same size, centered, wraps for expanded label
        m_renderer.DWrite()->CreateTextFormat(
            L"Segoe UI Variable", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            lblPx, L"", &m_iconLabelFullFormat);
        if (m_iconLabelFullFormat)
        {
            m_iconLabelFullFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_iconLabelFullFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            m_iconLabelFullFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        // Opaque background for expanded label popup
        dc->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.13f, 0.22f, 0.97f), &m_iconLabelBgBrush);
    }

    // ---- Drop handling ----

    void OnItemsDropped(const std::vector<std::pair<std::wstring,std::wstring>>& items)
    {
        bool changed = false;
        for (auto& [parsingName, fsPath] : items)
        {
            // Skip duplicates
            bool exists = false;
            for (auto& e : m_data.icons)
                if (_wcsicmp(e.parsingName.c_str(), parsingName.c_str()) == 0)
                    { exists = true; break; }
            if (exists) continue;

            FenceIconEntry entry;
            entry.path        = fsPath.empty() ? parsingName : fsPath;
            entry.parsingName = parsingName;
            entry.displayName = GetDisplayName(entry.path);

            // Also skip if this is literally the same file (e.g. dragged twice)
            // parsingName check above handles exact path match;
            // this handles the case where the file was already moved to our data folder
            {
                std::wstring dataFolder = FileOps::GetFenceDataFolder(m_data.id);
                std::wstring srcFolder  = std::filesystem::path(parsingName)
                                            .parent_path().wstring();
                if (!dataFolder.empty() &&
                    _wcsicmp(srcFolder.c_str(), dataFolder.c_str()) == 0)
                    continue; // already in our data folder - skip
            }

            if (FileOps::IsMovable(parsingName))
            {
                // Queue the physical move - Explorer still holds the file locked
                // during Drop(). PostMessage defers until Explorer releases it.
                m_pendingMoves.push_back(parsingName);
            }

            m_data.icons.push_back(std::move(entry));
            changed = true;
        }

        if (changed)
        {
            SortIconsByName(m_data.icons);
            m_dropHighlight = false;
            Render();
            if (m_msgWnd_external)
                PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);

            // Deferred move: fires after Explorer finishes its Drop() handling
            if (!m_pendingMoves.empty())
                PostMessageW(m_hwnd, WM_APP + 55, 0, 0);
        }
    }

    void ProcessPendingMoves()
    {
        if (m_pendingMoves.empty()) return;

        bool changed = false;
        for (const std::wstring& oldPath : m_pendingMoves)
        {
            // Find the icon entry that has this parsingName
            for (auto& e : m_data.icons)
            {
                if (_wcsicmp(e.parsingName.c_str(), oldPath.c_str()) != 0) continue;

                std::wstring newPath = FileOps::MoveToFence(oldPath, m_data.id);
                if (!newPath.empty())
                {
                    if (m_iconCache) m_iconCache->Invalidate(oldPath);
                    e.path        = newPath;
                    e.parsingName = newPath;
                    // Update display name to reflect renamed file (e.g. "doc (1).txt")
                    e.displayName = std::filesystem::path(newPath).filename().wstring();
                    changed = true;
                }
                else
                {
                    DebugLog(L"[WinFences] MoveToFence failed for: %s", oldPath.c_str());
                }
                break;
            }
        }
        m_pendingMoves.clear();

        if (changed)
        {
            Render();
            if (m_msgWnd_external)
                PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);
        }
    }

    // Convert screen point to nearest drop slot index (insert-before semantics)
    int PointToDropIndex(POINTL ptScreen) const
    {
        float scale = DpiHelper::ScaleForWindow(m_hwnd);

        POINT pt = { ptScreen.x, ptScreen.y };
        ScreenToClient(m_hwnd, &pt);

        int cell  = static_cast<int>(Grid_CellPx(scale) * scale);  // physical
        int margPhys = static_cast<int>(Grid_MarginPx()  * scale);
        int headPhys = static_cast<int>(Grid_HeaderPx()  * scale);
        int col = imax(0, imin(m_data.cols - 1,
            (pt.x - margPhys + cell/2) / cell));
        int row = imax(0, imin(m_data.rows - 1,
            (pt.y - headPhys - margPhys + cell/2) / cell));

        return imin(static_cast<int>(m_data.icons.size()), row * m_data.cols + col);
    }

    // ---- Hit testing ----

    // Returns the icon index under a client-coords point, or -1 if none
    int PointToIconIndex(POINT physPt) const
    {
        return PixelToIconIndex(physPt.x, physPt.y,
            m_data.cols, m_data.rows,
            DpiHelper::ScaleForWindow(m_hwnd));
    }

    enum class HitZone { None, TitleBar, ResizeSE };

    HitZone HitTest(POINT pt) const
    {
        RECT rc; GetClientRect(m_hwnd, &rc);
        float scale  = DpiHelper::ScaleForWindow(m_hwnd);
        int labelPx  = static_cast<int>(Grid_HeaderPx() * scale);
        int gripPx   = static_cast<int>(12 * scale);

        if (pt.x >= rc.right - gripPx && pt.y >= rc.bottom - gripPx) return HitZone::ResizeSE;
        if (pt.y < labelPx) return HitZone::TitleBar;
        return HitZone::None;
    }

    // ---- Snap window to grid on resize end ----

    void SnapWindowToGrid()
    {
        float scale = DpiHelper::ScaleForWindow(m_hwnd);
        // col/row/cols/rows are the truth — re-apply window position/size from them
        SetWindowPos(m_hwnd, nullptr,
            FenceWindowX(m_data.col,  scale),
            FenceWindowY(m_data.row,  scale),
            FenceWindowW(m_data.cols, scale),
            FenceWindowH(m_data.rows, scale),
            SWP_NOZORDER | SWP_NOACTIVATE);

        m_renderer.Resize();
        CreateRenderResources();
        Render();
    }

    // ---- Context menu ----

    void ShowFenceContextMenu(POINT screenPt)
    {
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, 1, L"Rename");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, 2, L"Save Snapshot");
        AppendMenuW(hMenu, MF_STRING, 3, L"Restore Snapshot");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, 4, L"Delete Fence");

        SetForegroundWindow(m_hwnd);
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPt.x, screenPt.y, 0, m_hwnd, nullptr);
        DestroyMenu(hMenu);

        switch (cmd)
        {
        case 1: PromptRename(); break;
        case 2: if (m_msgWnd_external) SendMessage(m_msgWnd_external, WM_APP+1, 0, 0); break;
        case 3: if (m_msgWnd_external) SendMessage(m_msgWnd_external, WM_APP+2, 0, 0); break;
        case 4: if (m_msgWnd_external) SendMessage(m_msgWnd_external, WM_APP+3, reinterpret_cast<WPARAM>(m_hwnd), 0); break;
        }
    }

    void PromptRename()
    {
        // Simple rename via InputBox-style dialog using a child window
        // We create a small popup with an edit control over the label bar
        struct Dlg {
            static INT_PTR CALLBACK Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
            {
                switch (msg)
                {
                case WM_INITDIALOG:
                {
                    // Center over parent
                    HWND hParent = GetParent(hDlg);
                    RECT pr, dr;
                    GetWindowRect(hParent, &pr);
                    GetWindowRect(hDlg, &dr);
                    int w = dr.right - dr.left;
                    int h = dr.bottom - dr.top;
                    int x = pr.left + (pr.right - pr.left - w) / 2;
                    int y = pr.top  + (pr.bottom - pr.top - h) / 2;
                    SetWindowPos(hDlg, nullptr, x, y, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER);

                    HWND hEdit = GetDlgItem(hDlg, 100);
                    // Pre-fill with current label passed via lParam
                    const wchar_t* cur = reinterpret_cast<const wchar_t*>(lp);
                    SetWindowTextW(hEdit, cur ? cur : L"");
                    SendMessageW(hEdit, EM_SETSEL, 0, -1);
                    SetFocus(hEdit);
                    return FALSE;
                }
                case WM_COMMAND:
                    if (LOWORD(wp) == IDOK)
                    {
                        wchar_t buf[256] = {};
                        GetDlgItemTextW(hDlg, 100, buf, 256);
                        // Store result in window prop
                        wchar_t* result = new wchar_t[256];
                        wcscpy_s(result, 256, buf);
                        SetPropW(hDlg, L"Result", result);
                        EndDialog(hDlg, IDOK);
                    }
                    else if (LOWORD(wp) == IDCANCEL)
                        EndDialog(hDlg, IDCANCEL);
                    return TRUE;
                case WM_KEYDOWN:
                    if (wp == VK_RETURN)  { SendMessageW(hDlg, WM_COMMAND, IDOK,     0); return TRUE; }
                    if (wp == VK_ESCAPE)  { SendMessageW(hDlg, WM_COMMAND, IDCANCEL, 0); return TRUE; }
                    break;
                }
                return FALSE;
            }
        };

        // Build dialog template in memory
        #pragma pack(push, 1)
        struct DlgTmpl {
            DLGTEMPLATE hdr;
            WORD menu, cls, title;
            // Edit control
            DLGITEMTEMPLATE edit;
            WORD editCls[2];  // 0xFFFF, 0x0081 = EDIT
            WORD editText;
            WORD editExtra;
            // OK button
            DLGITEMTEMPLATE ok;
            WORD okCls[2];
            WORD okText[3];   // "OK "
            WORD okExtra;
            // Cancel button
            DLGITEMTEMPLATE cancel;
            WORD cancelCls[2];
            WORD cancelText[8]; // "Cancel "
            WORD cancelExtra;
        };
        #pragma pack(pop)

        // Use MessageBox + GetWindowText pattern instead - simpler and reliable
        // Create a temporary floating edit window
        HWND hEdit = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"EDIT", m_data.label.c_str(),
            WS_POPUP | WS_BORDER | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 300, 28,
            m_hwnd, nullptr, m_hInst, nullptr);

        if (!hEdit) return;

        // Position over the label bar
        RECT wr;
        GetWindowRect(m_hwnd, &wr);
        float scale = DpiHelper::ScaleForWindow(m_hwnd);
        int labelH = static_cast<int>(Grid_HeaderPx() * scale);
        SetWindowPos(hEdit, HWND_TOPMOST,
            wr.left, wr.top, wr.right - wr.left, labelH + 4,
            SWP_SHOWWINDOW);

        // Set font matching the label bar
        HFONT hFont = reinterpret_cast<HFONT>(
            SendMessageW(hEdit, WM_GETFONT, 0, 0));
        LOGFONTW lf = {};
        lf.lfHeight = -MulDiv(11, GetDeviceCaps(GetDC(hEdit), LOGPIXELSY), 72);
        lf.lfWeight = FW_SEMIBOLD;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable");
        hFont = CreateFontIndirectW(&lf);
        SendMessageW(hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

        SendMessageW(hEdit, EM_SETSEL, 0, -1);
        SetFocus(hEdit);

        // Low-level hook to catch clicks outside the edit (WS_EX_NOACTIVATE
        // means outside clicks never arrive in our queue otherwise).
        // Use a plain static local so the callback can access it without
        // needing access to the outer class scope.
        static HWND s_renameEditHwnd = nullptr;
        s_renameEditHwnd = hEdit;

        HHOOK renameHook = SetWindowsHookExW(WH_MOUSE_LL,
            [](int nCode, WPARAM wp, LPARAM lp) -> LRESULT {
                if (nCode == HC_ACTION && s_renameEditHwnd
                    && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN))
                {
                    auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
                    if (WindowFromPoint(info->pt) != s_renameEditHwnd)
                        PostMessageW(s_renameEditHwnd, WM_APP + 57, 0, 0);
                }
                return CallNextHookEx(nullptr, nCode, wp, lp);
            },
            nullptr, 0);

        MSG msg = {};
        bool accepted = false;
        while (IsWindow(hEdit))
        {
            if (!GetMessageW(&msg, nullptr, 0, 0)) break;
            if (msg.message == WM_KEYDOWN)
            {
                if (msg.wParam == VK_RETURN) { accepted = true;  break; }
                if (msg.wParam == VK_ESCAPE) { accepted = false; break; }
            }
            // WM_APP+57 = click outside (from hook above)
            if (msg.message == WM_APP + 57) { accepted = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (renameHook) UnhookWindowsHookEx(renameHook);
        s_renameEditHwnd = nullptr;

        if (accepted)
        {
            wchar_t buf[256] = {};
            GetWindowTextW(hEdit, buf, 256);
            if (wcslen(buf) > 0)
            {
                m_data.label = buf;
                Render();
                if (m_msgWnd_external)
                    PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);
            }
        }

        DestroyWindow(hEdit);
        if (hFont) DeleteObject(hFont);
    }

    // ---- Window procedure ----

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        FenceWindow* self = nullptr;
        if (msg == WM_NCCREATE)
        {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<FenceWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->m_hwnd = hwnd;
        }
        else
            self = reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (self) return self->WndProc(hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_WINDOWPOSCHANGING:
        {
            WINDOWPOS* wp2 = reinterpret_cast<WINDOWPOS*>(lp);
            if (wp2->flags & SWP_HIDEWINDOW)
                wp2->flags &= ~SWP_HIDEWINDOW;
            if (!(wp2->flags & SWP_NOZORDER) && wp2->hwndInsertAfter != HWND_BOTTOM)
                wp2->flags |= SWP_NOZORDER;
            break;
        }
        case WM_WINDOWPOSCHANGED:
        {
            if (!IsWindowVisible(hwnd))
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            break;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
            Render();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZING:
        {
            m_isSizing = true;
            float scale = DpiHelper::ScaleForWindow(hwnd);
            RECT* rc = reinterpret_cast<RECT*>(lp);
            int cols = PixelToCols(rc->right - rc->left, scale);
            int rows = PixelToRows(rc->bottom - rc->top,  scale);
            rc->right  = rc->left + FenceWindowW(cols, scale);
            rc->bottom = rc->top  + FenceWindowH(rows, scale);
            return TRUE;
        }



        case WM_SIZE:
            m_renderer.Resize();
            CreateRenderResources();
            Render();
            return 0;

        case WM_DPICHANGED:
        {
            auto newRect = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr,
                newRect->left, newRect->top,
                newRect->right - newRect->left, newRect->bottom - newRect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            m_renderer.Resize();
            CreateRenderResources();
            if (m_iconCache) m_iconCache->Clear();
            float newScale = DpiHelper::ScaleForWindow(hwnd);
            GetDesktopIconSizePx(newScale, true); // invalidate icon size cache
            SnapWindowToGrid();
            return 0;
        }

        case WM_MOVING:
        {
            // Snap to 20px grid on release (WM_EXITSIZEMOVE), not during drag
            // WM_MOVING with TRUE blocks mouse - let Windows handle movement freely
            break;
        }



        case WM_NCHITTEST:
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            auto zone = HitTest(pt);
            if (zone == HitZone::TitleBar)  return HTCAPTION;
            if (zone == HitZone::ResizeSE)  return HTBOTTOMRIGHT;
            return HTCLIENT;
        }

        case WM_EXITSIZEMOVE:
        {
            // PRINCIPLE: col/row is the truth. Pixels are derived, never the source.
            // 1. Read current pixel position/size (only time we read pixels)
            // 2. Convert to nearest col/row (snap)
            // 3. Derive exact pixels back from col/row
            // 4. Apply to window — now window IS the grid

            float scale = DpiHelper::ScaleForWindow(hwnd);
            RECT wrc; GetWindowRect(hwnd, &wrc);

            // col/row from physical window position — snap to nearest cell
            m_data.col = PixelToCol(wrc.left, scale);
            m_data.row = PixelToRow(wrc.top,  scale);
            if (m_isSizing)
            {
                m_data.cols = PixelToCols(wrc.right - wrc.left, scale);
                m_data.rows = PixelToRows(wrc.bottom - wrc.top, scale);
            }
            m_wasSizing = m_isSizing;
            m_isSizing  = false;

            // Derive exact pixels from col/row and apply
            SetWindowPos(hwnd, nullptr,
                FenceWindowX(m_data.col,  scale),
                FenceWindowY(m_data.row,  scale),
                FenceWindowW(m_data.cols, scale),
                FenceWindowH(m_data.rows, scale),
                SWP_NOZORDER | SWP_NOACTIVATE);
            m_renderer.Resize();

            DebugLog(L"[Snap] col=%d row=%d cols=%d rows=%d",
                m_data.col, m_data.row, m_data.cols, m_data.rows);

            if (m_msgWnd_external)
                PostMessageW(m_msgWnd_external, WM_APP + 59,
                    reinterpret_cast<WPARAM>(hwnd), 0);
            Render();
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = PointToIconIndex(pt);
            if (m_selectedIndex != idx)
            {
                m_selectedIndex = idx;
                if (idx >= 0) InstallMouseHook();
                else          UninstallMouseHook();
                Render();
            }
            // Arm drag via hook (WS_EX_NOACTIVATE windows don't get
            // WM_MOUSEMOVE reliably outside their bounds, so we track
            // with the global mouse hook that's already installed)
            if (idx >= 0 && idx < static_cast<int>(m_data.icons.size()))
            {
                m_dragPending = true;
                m_dragIndex   = idx;
                POINT screenPt = pt;
                ClientToScreen(hwnd, &screenPt);
                m_dragStartPt = screenPt;
                InstallMouseHook(); // hook tracks move+up globally
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            m_dragPending = false;
            m_dragIndex   = -1;
            return 0;

        case WM_LBUTTONDBLCLK:
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = PointToIconIndex(pt);
            if (idx >= 0 && idx < static_cast<int>(m_data.icons.size()))
                ShellActions::Launch(hwnd, m_data.icons[idx].parsingName);
            return 0;
        }

        case WM_NCRBUTTONUP:
            // Right-click on non-client area (title bar = HTCAPTION)
            if (wp == HTCAPTION)
            {
                POINT screenPt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                ShowFenceContextMenu(screenPt);
                return 0;
            }
            break;

        case WM_RBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = PointToIconIndex(pt);
            POINT screenPt = pt;
            ClientToScreen(hwnd, &screenPt);

            if (idx >= 0 && idx < static_cast<int>(m_data.icons.size()))
            {
                m_selectedIndex = idx;
                InstallMouseHook();
                Render();
                // Copy parsingName before ShowContextMenu - idx may be stale after return
                // Use current parsingName from icon (may have been updated by move)
                std::wstring menuPath = m_data.icons[idx].parsingName;
                ShellActions::ShowContextMenu(hwnd, menuPath, screenPt);
                // Always prune after menu closes - handles delete, rename, move
                PruneDeletedIcons();
                m_selectedIndex = -1;
                UninstallMouseHook();
                Render();
            }
            else
            {
                m_selectedIndex = -1;
                UninstallMouseHook();
                Render();
                ShowFenceContextMenu(screenPt);
            }
            return 0;
        }

        case WM_APP + 50: // DragOver - update highlight index
        {
            POINTL ptl = { static_cast<LONG>(wp), static_cast<LONG>(lp) };
            m_dropHighlight      = true;
            m_dropHighlightIndex = PointToDropIndex(ptl);
            Render();
            return 0;
        }

        case WM_APP + 51: // DragLeave
            m_dropHighlight = false;
            Render();
            return 0;

        case WM_SETTINGCHANGE:
            // Fires when Explorer icon size changes or shell state changes
            GetDesktopIconSizePx(DpiHelper::ScaleForWindow(hwnd), true);
            PruneDeletedIcons(); // remove icons whose files were deleted
            SnapWindowToGrid();
            return 0;


        case WM_APP + 52: // Deferred startup render (message pump now active)
            GetDesktopIconSizePx(DpiHelper::ScaleForWindow(hwnd), true);
            SyncDataFolder(); // bidirectional: add orphan files, prune missing icons
            SnapWindowToGrid();
            Render();
            return 0;

        case WM_APP + 53: // Deselect triggered by click outside this window
            if (m_selectedIndex != -1)
            {
                m_selectedIndex = -1;
                UninstallMouseHook();
                Render();
            }
            return 0;

        case WM_APP + 56: // Deferred prune after context menu close
            PruneDeletedIcons();
            return 0;

        case WM_APP + 58: // FileWatcher or ShellNotify: file changed in data folder
            SyncDataFolder(); // add new files + remove deleted ones
            return 0;


        case WM_APP + 54: // Begin drag-out (posted from low-level mouse hook)
        {
            int dragIdx = static_cast<int>(wp);
            UninstallMouseHook();
            BeginDragOut(dragIdx);
            return 0;
        }

        case WM_APP + 55: // Process deferred file moves (after Explorer releases lock)
            ProcessPendingMoves();
            return 0;

        case WM_DESTROY:
            m_fileWatcher.Stop();
            UnregisterShellNotify();
            UninstallMouseHook();
            RevokeDragDrop(hwnd);
            m_hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};
