#pragma once
// ============================================================================
// FenceWindow.h � One fence: a Win32 window that displays a grid of icons.
//
// Each fence is a layered WS_POPUP window with DWM glass background.
// All layout is driven by FenceData.col/row/cols/rows via GridSystem.h.
//
// Key responsibilities:
//   Rendering  � Direct2D: background, header bar, icon bitmaps, labels,
//                selection highlight, drop highlight, expanded label popup
//   Input      � drag-to-move (title bar), resize (edges/corners via
//                WM_NCHITTEST), icon click/double-click/right-click
//   Snap       � WM_EXITSIZEMOVE converts physical pixel position back to
//                col/row (PixelToCol/Row), then re-derives canonical pixels
//                (FenceWindowX/Y/W/H) � grid is always the truth
//   OLE D&D    � FenceDropTarget (drop in), FenceDragSource (drag out)
//   File sync  � FileWatcher triggers re-scan of data folder
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

        // Z-order bottom BEFORE showing � prevents Windows from raising us on first paint
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

        // Place at Z-order bottom immediately � WM_WINDOWPOSCHANGING then
        // prevents anyone from raising us. (NoFences technique)
        SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(m_hwnd, &margins);

        HRESULT hr = m_renderer.Initialize(m_hwnd);
        if (FAILED(hr)) { DebugLog(L"Renderer init failed: 0x%08X", hr); return false; }

        // Snap to grid on creation � size from col/row counts, position from grid
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
            [this](const std::vector<std::pair<std::wstring,std::wstring>>& items,
                   DropAction action)
            { OnItemsDropped(items, action); });
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

    // Hide/show for the desktop double-click toggle. m_hidden is what tells
    // WM_WINDOWPOSCHANGING to allow the hide it otherwise blocks. Deliberately
    // not part of FenceData: the state is per-session and never persisted, so
    // every fence is visible again after a restart.
    void SetHidden(bool hidden)
    {
        if (m_hidden == hidden || !m_hwnd) return;
        m_hidden = hidden;

        if (hidden)
        {
            // Drop any selection first, so its global hooks do not stay live
            // while the fence is invisible.
            m_selectedIndex = -1;
            UninstallSelectionHooks();
            ShowWindow(m_hwnd, SW_HIDE);
        }
        else
        {
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            Render();
        }
    }

    bool IsHidden() const { return m_hidden; }

    FenceData GetData() const
    {
        // col/row/cols/rows in m_data ARE the truth � always kept up to date.
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
        if (m_bgBrush) { m_bgBrush->SetOpacity(m_data.alpha); dc->FillRoundedRectangle(bgRect, m_bgBrush.Get()); }
        // Border is stroked AFTER the header bar below, not here — see there.

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

        // ---- Border, last ----
        // A 1px stroke is centred on the path, so half of it lies inside the
        // clip geometry the header bar is filled against. Stroking before that
        // fill let the header paint over its inner half: the outline vanished
        // along the top and the upper sides and only survived below the header.
        // Drawing it after everything keeps one even line the whole way round.
        if (m_borderBrush) dc->DrawRoundedRectangle(bgRect, m_borderBrush.Get(), 1.0f);

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

            FenceIconEntry* existing = nullptr;
            for (auto& icon : m_data.icons)
                if (_wcsicmp(icon.parsingName.c_str(), path.c_str()) == 0)
                    { existing = &icon; break; }

            if (existing)
            {
                // Re-derive the label instead of trusting the snapshot: labels
                // are persisted in autosave.json, so an entry written by an
                // older build keeps its stale name (e.g. "Dokument.lnk")
                // forever otherwise.
                std::wstring label = GetFenceLabel(path);
                if (existing->displayName != label)
                {
                    DebugLog(L"[Sync] relabel: %s -> %s",
                        existing->displayName.c_str(), label.c_str());
                    existing->displayName = label;
                    changed = true;
                }
                continue;
            }

            FenceIconEntry e;
            e.path        = path;
            e.parsingName = path;
            e.displayName = GetFenceLabel(path);
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
    bool             m_hidden            = false; // desktop double-click toggle
    int              m_dropHighlightIndex = -1;
    int              m_selectedIndex      = -1;  // icon selection highlight

    // Global mouse hook - active only while an icon is selected
    // Detects clicks outside this window so we can deselect
    HHOOK            m_mouseHook          = nullptr;
    HHOOK            m_keyHook            = nullptr;

    // Shell change notification cookie
    ULONG m_shellNotifyCookie = 0;

    // FileSystem watcher for data folder (catches Cut, Delete, Rename)
    FileWatcher m_fileWatcher;

    // Pending file operations, (sourcePath, action) — deferred until after
    // Explorer releases the file lock it holds during Drop().
    std::vector<std::pair<std::wstring, DropAction>> m_pendingOps;

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
        UninstallSelectionHooks();
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

    // Delete key on the selected icon: removes THAT item and nothing else.
    // For a shortcut that means the .lnk in the fence's data folder — the file
    // it points at is never touched.
    void DeleteSelectedIcon()
    {
        if (m_selectedIndex < 0
            || m_selectedIndex >= static_cast<int>(m_data.icons.size()))
            return;

        const std::wstring target = m_data.icons[m_selectedIndex].parsingName;

        // Drop the selection (and with it the hooks) before the shell dialog
        // runs, so no keystroke is swallowed while the operation is in flight.
        m_selectedIndex = -1;
        UninstallSelectionHooks();

        if (FileOps::IsMovable(target))
        {
            if (!ShellActions::DeleteToRecycleBin(m_hwnd, target))
            {
                DebugLog(L"[Delete] failed for: %s", target.c_str());
                Render();
                return;
            }
            if (m_iconCache) m_iconCache->Invalidate(target);
            PruneDeletedIcons(); // file is gone -> entry goes with it
        }
        else
        {
            // Virtual shell item (Store app, ::{CLSID}): nothing on disk to
            // delete, so Delete just takes it out of the fence.
            for (int i = 0; i < static_cast<int>(m_data.icons.size()); ++i)
                if (_wcsicmp(m_data.icons[i].parsingName.c_str(), target.c_str()) == 0)
                {
                    m_data.icons.erase(m_data.icons.begin() + i);
                    break;
                }
            if (m_msgWnd_external)
                PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);
        }

        Render();
    }

    // Arrow-key navigation inside the fence. Icons sit in a row-major grid of
    // m_data.cols columns, so left/right step by one and up/down by a row.
    // Movement stops at the edges rather than wrapping — a fence is a small,
    // fully visible grid, and wrapping there reads as the selection jumping.
    void MoveSelection(UINT vk)
    {
        const int count = static_cast<int>(m_data.icons.size());
        if (count == 0 || m_selectedIndex < 0) return;

        const int cols = imax(1, m_data.cols);
        int idx = m_selectedIndex;

        switch (vk)
        {
        case VK_LEFT:  idx -= 1;    break;
        case VK_RIGHT: idx += 1;    break;
        case VK_UP:    idx -= cols; break;
        case VK_DOWN:  idx += cols; break;
        default: return;
        }

        if (idx < 0 || idx >= count || idx == m_selectedIndex) return;

        m_selectedIndex = idx;
        Render();
    }

    void LaunchSelectedIcon()
    {
        if (m_selectedIndex < 0
            || m_selectedIndex >= static_cast<int>(m_data.icons.size()))
            return;
        ShellActions::Launch(m_hwnd, m_data.icons[m_selectedIndex].parsingName);
    }

    // ---- Selection hooks ----
    //
    // A fence is WS_EX_NOACTIVATE and is shown with SW_SHOWNOACTIVATE, so
    // clicking an icon never gives the window keyboard focus and it never
    // receives WM_KEYDOWN. Without the keyboard hook below, pressing Delete
    // over a selected fence icon goes to whatever window actually holds focus —
    // normally the desktop — which then deletes ITS OWN selected item. That is
    // how a Delete aimed at a fence shortcut ended up deleting a desktop file.
    //
    // Mouse and keyboard hook share one lifetime and one s_hookTarget, so they
    // are always installed and removed together.

    void InstallSelectionHooks()
    {
        // Store this pointer in a static so the static procs can reach it
        s_hookTarget = this;

        // Remember who held the foreground, so the hook can tell later whether
        // the user has moved on (see SelectionStillOwnsKeyboard).
        s_fgAtSelection = GetForegroundWindow();

        if (!m_mouseHook)
            m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                GetModuleHandleW(nullptr), 0);

        if (!m_keyHook)
            m_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                GetModuleHandleW(nullptr), 0);
    }

    void UninstallSelectionHooks()
    {
        if (m_mouseHook) { UnhookWindowsHookEx(m_mouseHook); m_mouseHook = nullptr; }
        if (m_keyHook)   { UnhookWindowsHookEx(m_keyHook);   m_keyHook   = nullptr; }

        // Only give up the shared target if it is still ours. Clicking from one
        // fence to another posts the first fence's deselect (WM_APP+53), and
        // PostMessage is asynchronous: that deselect can run AFTER the second
        // fence has already claimed s_hookTarget. Clearing it unconditionally
        // there left the second fence with live hooks but no target, so Delete
        // and F2 silently stopped working on it.
        if (s_hookTarget == this) s_hookTarget = nullptr;
    }

    static inline FenceWindow* s_hookTarget = nullptr;

    // Foreground window at the moment the selection was made. The keyboard hook
    // is global, so it needs a rule for when a fence may claim a key.
    //
    // Requiring the desktop to be foreground does not work: a fence is
    // WS_EX_NOACTIVATE, so clicking one never changes the foreground window, and
    // the user usually clicks a fence straight out of whatever app they were in.
    // The desktop would then never be foreground and the keys would never arrive.
    //
    // What actually matters is that the user has not gone somewhere else since
    // selecting. Clicking anywhere outside the fence already clears the selection
    // (WM_APP+53), so the only remaining way to leave is Alt+Tab — and that
    // changes the foreground window. Comparing against the window that was
    // foreground at selection time covers exactly that case, without stealing
    // focus from anyone.
    static inline HWND s_fgAtSelection = nullptr;

    static bool SelectionStillOwnsKeyboard()
    {
        return GetForegroundWindow() == s_fgAtSelection;
    }

    // Keys that act on the selected icon, mirroring Explorer: arrows move the
    // selection, Enter launches, F2 renames, Delete recycles. Each is swallowed
    // so it cannot also reach the desktop behind the fence. Everything else,
    // and everything outside the desktop, is passed straight through.
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wp, LPARAM lp)
    {
        if (nCode == HC_ACTION && s_hookTarget
            && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)
            && s_hookTarget->m_selectedIndex >= 0
            && SelectionStillOwnsKeyboard())
        {
            auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
            HWND target = s_hookTarget->m_hwnd;

            switch (info->vkCode)
            {
            case VK_DELETE:
                PostMessageW(target, WM_APP + 62, 0, 0);
                return 1; // eat it — do NOT let the desktop act on this

            case VK_F2:
                PostMessageW(target, WM_APP + 63,
                    static_cast<WPARAM>(s_hookTarget->m_selectedIndex), 0);
                return 1;

            case VK_RETURN:
                PostMessageW(target, WM_APP + 64, 0, 0);
                return 1;

            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN:
                PostMessageW(target, WM_APP + 65,
                    static_cast<WPARAM>(info->vkCode), 0);
                return 1;
            }
        }
        return CallNextHookEx(nullptr, nCode, wp, lp);
    }

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

        // Body, header bar and border all derive from the fence's accent colour
        // so that picking one colour retints the fence as a whole. The header
        // bar IS the accent colour; the body is a darkened version of it and the
        // border a lightened one.
        const float ar = GetRValue(m_data.color) / 255.0f;
        const float ag = GetGValue(m_data.color) / 255.0f;
        const float ab = GetBValue(m_data.color) / 255.0f;

        auto shade = [&](float f, float a) { return D2D1::ColorF(ar*f, ag*f, ab*f, a); };
        auto tint  = [&](float t, float a) {
            return D2D1::ColorF(ar + (1.0f-ar)*t, ag + (1.0f-ag)*t, ab + (1.0f-ab)*t, a);
        };

        dc->CreateSolidColorBrush(shade(0.70f, m_data.alpha),        &m_bgBrush);
        dc->CreateSolidColorBrush(tint (0.52f, 0.85f),               &m_borderBrush);
        dc->CreateSolidColorBrush(D2D1::ColorF(ar, ag, ab, 0.92f),   &m_labelBgBrush);
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &m_textBrush);

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

    void OnItemsDropped(const std::vector<std::pair<std::wstring,std::wstring>>& items,
                        DropAction action)
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
            entry.displayName = GetFenceLabel(entry.path);

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
                // Queue the physical operation - Explorer still holds the file
                // locked during Drop(). PostMessage defers until it lets go.
                m_pendingOps.push_back({ parsingName, action });
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

            // Deferred op: fires after Explorer finishes its Drop() handling
            if (!m_pendingOps.empty())
                PostMessageW(m_hwnd, WM_APP + 55, 0, 0);
        }
    }

    // Runs the queued move/copy/shortcut operations and repoints each icon
    // entry at the file that now lives in this fence's data folder.
    void ProcessPendingOps()
    {
        if (m_pendingOps.empty()) return;

        bool changed = false;
        for (const auto& [oldPath, action] : m_pendingOps)
        {
            // Find the icon entry that has this parsingName
            for (auto& e : m_data.icons)
            {
                if (_wcsicmp(e.parsingName.c_str(), oldPath.c_str()) != 0) continue;

                std::wstring newPath = FileOps::ApplyToFence(oldPath, m_data.id, action);
                if (!newPath.empty())
                {
                    if (m_iconCache) m_iconCache->Invalidate(oldPath);
                    e.path        = newPath;
                    e.parsingName = newPath;
                    // Re-query: reflects a collision rename (e.g. "doc (1)") and
                    // drops the ".lnk" of a freshly created shortcut.
                    e.displayName = GetFenceLabel(newPath);
                    changed = true;
                }
                else
                {
                    DebugLog(L"[WinFences] Drop op failed for: %s", oldPath.c_str());
                }
                break;
            }
        }
        m_pendingOps.clear();

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
        // col/row/cols/rows are the truth � re-apply window position/size from them
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
        AppendMenuW(hMenu, MF_STRING, 5, L"Choose Color...");
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
        case 5: PromptChooseColor(); break;
        case 2: if (m_msgWnd_external) SendMessage(m_msgWnd_external, WM_APP+1, 0, 0); break;
        case 3: if (m_msgWnd_external) SendMessage(m_msgWnd_external, WM_APP+2, 0, 0); break;
        case 4: if (m_msgWnd_external) SendMessage(m_msgWnd_external, WM_APP+3, reinterpret_cast<WPARAM>(m_hwnd), 0); break;
        }
    }

    // Standard Windows colour dialog, opened with the fence's current colour
    // preselected. Custom swatches are static so they survive across fences and
    // across repeated use within a session.
    void PromptChooseColor()
    {
        static COLORREF s_customColors[16] = {
            RGB(46,56,115), RGB(38,74,54),  RGB(92,44,44),  RGB(72,52,96),
            RGB(30,30,30),  RGB(24,58,84),  RGB(96,72,28),  RGB(52,52,60),
            RGB(255,255,255), RGB(255,255,255), RGB(255,255,255), RGB(255,255,255),
            RGB(255,255,255), RGB(255,255,255), RGB(255,255,255), RGB(255,255,255),
        };

        CHOOSECOLORW cc = { sizeof(cc) };
        cc.hwndOwner    = m_hwnd;
        cc.rgbResult    = m_data.color;
        cc.lpCustColors = s_customColors;
        cc.Flags        = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;

        if (!ChooseColorW(&cc)) return; // cancelled

        m_data.color = cc.rgbResult;
        CreateRenderResources(); // rebuild the brushes from the new accent
        Render();

        // Persist: autosave writes "color" into the fence's snapshot entry.
        if (m_msgWnd_external)
            PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);
    }

    // Rename the fence itself: inline editor over its header bar.
    void PromptRename()
    {
        RECT wr;
        GetWindowRect(m_hwnd, &wr);
        float scale = DpiHelper::ScaleForWindow(m_hwnd);
        int labelH = static_cast<int>(Grid_HeaderPx() * scale);
        RECT editRc = { wr.left, wr.top, wr.right, wr.top + labelH + 4 };

        std::wstring edited;
        if (!PromptInlineEdit(m_data.label, editRc, 11, FW_SEMIBOLD, edited)) return;
        if (edited.empty()) return;

        m_data.label = edited;
        Render();
        if (m_msgWnd_external)
            PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);
    }

    // Rename the file behind an icon. A shortcut is renamed as the .lnk it is;
    // its target is never touched. Explorer supplies this through its folder
    // view, which a fence does not have, so the editor is ours.
    void PromptRenameIcon(int idx)
    {
        if (idx < 0 || idx >= static_cast<int>(m_data.icons.size())) return;

        const std::wstring oldPath = m_data.icons[idx].parsingName;
        if (!FileOps::IsMovable(oldPath))
        {
            // Virtual shell item (Store app, ::{CLSID}) — nothing on disk.
            DebugLog(L"[Rename] not a file, ignored: %s", oldPath.c_str());
            return;
        }

        std::filesystem::path p(oldPath);
        const std::wstring ext = p.extension().wstring();
        const bool isLnk = _wcsicmp(ext.c_str(), L".lnk") == 0;

        // Offer exactly the text the fence shows, i.e. a shortcut without its
        // ".lnk", and put that extension back on the way in.
        const std::wstring shown = GetFenceLabel(oldPath);

        // Editor over the icon's label, widened so long names stay editable
        float scale = DpiHelper::ScaleForWindow(m_hwnd);
        D2D1_RECT_F lbl = LabelRect(idx % m_data.cols, idx / m_data.cols, scale);
        int cellPhys = static_cast<int>(Grid_CellPx(scale) * scale);
        RECT rc = { static_cast<LONG>(lbl.left)  - cellPhys / 2,
                    static_cast<LONG>(lbl.top),
                    static_cast<LONG>(lbl.right) + cellPhys / 2,
                    static_cast<LONG>(lbl.top) + static_cast<LONG>(22 * scale) };
        MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);

        std::wstring edited;
        if (!PromptInlineEdit(shown, rc, 10, FW_NORMAL, edited)) return;
        if (edited == shown) return;

        const std::wstring newName = isLnk ? edited + ext : edited;
        if (!ShellActions::IsValidFileName(newName))
        {
            DebugLog(L"[Rename] rejected name: %s", newName.c_str());
            return;
        }

        std::wstring newPath = ShellActions::RenameItem(m_hwnd, oldPath, newName);
        if (newPath.empty())
        {
            DebugLog(L"[Rename] failed: %s -> %s", oldPath.c_str(), newName.c_str());
            return;
        }

        if (m_iconCache) m_iconCache->Invalidate(oldPath);
        for (auto& e : m_data.icons)
            if (_wcsicmp(e.parsingName.c_str(), oldPath.c_str()) == 0)
            {
                e.path        = newPath;
                e.parsingName = newPath;
                e.displayName = GetFenceLabel(newPath);
                break;
            }

        SortIconsByName(m_data.icons);
        m_selectedIndex = -1;
        UninstallSelectionHooks();
        Render();
        if (m_msgWnd_external)
            PostMessageW(m_msgWnd_external, WM_APP + 1, 0, 0);
    }

    // Floating single-line editor, shared by the fence label and icon renames.
    // Returns false if the user cancelled with Escape.
    //
    // The fence is WS_EX_NOACTIVATE, so a click outside never reaches our queue;
    // a mouse hook spots it and commits, the same way Explorer's inline rename
    // behaves.
    bool PromptInlineEdit(const std::wstring& initial, RECT screenRc,
                          int fontPt, int fontWeight, std::wstring& out)
    {
        HWND hEdit = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"EDIT", initial.c_str(),
            WS_POPUP | WS_BORDER | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 300, 28,
            m_hwnd, nullptr, m_hInst, nullptr);
        if (!hEdit) return false;

        SetWindowPos(hEdit, HWND_TOPMOST,
            screenRc.left, screenRc.top,
            screenRc.right - screenRc.left, screenRc.bottom - screenRc.top,
            SWP_SHOWWINDOW);

        HDC hdc = GetDC(hEdit);
        LOGFONTW lf = {};
        lf.lfHeight = -MulDiv(fontPt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        lf.lfWeight = fontWeight;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable");
        ReleaseDC(hEdit, hdc);
        HFONT hFont = CreateFontIndirectW(&lf);
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
            out = buf;
        }

        DestroyWindow(hEdit);
        if (hFont) DeleteObject(hFont);
        return accepted;
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
            // Normally we refuse to be hidden — Show Desktop and friends try it
            // constantly. m_hidden marks the one case where hiding is ours.
            if ((wp2->flags & SWP_HIDEWINDOW) && !m_hidden)
                wp2->flags &= ~SWP_HIDEWINDOW;
            if (!(wp2->flags & SWP_NOZORDER) && wp2->hwndInsertAfter != HWND_BOTTOM)
                wp2->flags |= SWP_NOZORDER;
            break;
        }
        case WM_WINDOWPOSCHANGED:
        {
            if (!m_hidden && !IsWindowVisible(hwnd))
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
            // 4. Apply to window � now window IS the grid

            float scale = DpiHelper::ScaleForWindow(hwnd);
            RECT wrc; GetWindowRect(hwnd, &wrc);

            // col/row from physical window position � snap to nearest cell
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
                if (idx >= 0) InstallSelectionHooks();
                else          UninstallSelectionHooks();
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
                InstallSelectionHooks(); // hook tracks move+up globally
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
                InstallSelectionHooks();
                Render();
                // Copy parsingName before ShowContextMenu - idx may be stale after return
                // Use current parsingName from icon (may have been updated by move)
                std::wstring menuPath = m_data.icons[idx].parsingName;
                bool renameable = FileOps::IsMovable(menuPath);
                auto res = ShellActions::ShowContextMenu(hwnd, menuPath, screenPt,
                                                         renameable);
                // Always prune after menu closes - handles delete, rename, move
                PruneDeletedIcons();

                if (res == ShellActions::MenuResult::Rename)
                {
                    // Re-find the icon: PruneDeletedIcons may have reordered.
                    int cur = -1;
                    for (int i = 0; i < static_cast<int>(m_data.icons.size()); ++i)
                        if (_wcsicmp(m_data.icons[i].parsingName.c_str(),
                                     menuPath.c_str()) == 0) { cur = i; break; }
                    m_selectedIndex = -1; // clear now: PromptRenameIcon only
                    UninstallSelectionHooks(); // does so when it succeeds
                    PromptRenameIcon(cur);
                    Render();
                    return 0;
                }

                m_selectedIndex = -1;
                UninstallSelectionHooks();
                Render();
            }
            else
            {
                m_selectedIndex = -1;
                UninstallSelectionHooks();
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
                UninstallSelectionHooks();
                Render();
            }
            return 0;

        case WM_APP + 56: // Deferred prune after context menu close
            PruneDeletedIcons();
            return 0;

        case WM_APP + 62: // Delete key on the selected icon (from the key hook)
            DeleteSelectedIcon();
            return 0;

        case WM_APP + 63: // F2 on the selected icon (from the key hook)
            UninstallSelectionHooks();
            PromptRenameIcon(static_cast<int>(wp));
            return 0;

        case WM_APP + 64: // Enter on the selected icon
            LaunchSelectedIcon();
            return 0;

        case WM_APP + 65: // Arrow key: move the selection (wp = virtual key)
            MoveSelection(static_cast<UINT>(wp));
            return 0;

        case WM_APP + 58: // FileWatcher or ShellNotify: file changed in data folder
            SyncDataFolder(); // add new files + remove deleted ones
            return 0;


        case WM_APP + 54: // Begin drag-out (posted from low-level mouse hook)
        {
            int dragIdx = static_cast<int>(wp);
            UninstallSelectionHooks();
            BeginDragOut(dragIdx);
            return 0;
        }

        case WM_APP + 55: // Process deferred file ops (after Explorer releases lock)
            ProcessPendingOps();
            return 0;

        case WM_DESTROY:
            m_fileWatcher.Stop();
            UnregisterShellNotify();
            UninstallSelectionHooks();
            RevokeDragDrop(hwnd);
            m_hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};
