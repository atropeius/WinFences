#pragma once
// ============================================================================
// FenceManager.h — Owns all fence windows and coordinates between them.
//
// Responsibilities:
//   - CreateFence / DeleteFence / CreateDefaultFence
//   - Load/save snapshots via SnapshotStore
//   - ResolveCollision: after every move/resize, ensures fences respect
//     FENCE_MIN_GAP (defined in GridSystem.h) on the grid
//   - SyncDataFolder: reconciles %APPDATA%/WinFences/data/ with FenceData
//     on startup and on FileWatcher events
//   - CleanOrphanFiles: removes leftover data folders for deleted fences
//   - ShellNotify: registers/unregisters SHChangeNotify for desktop changes
//
// Grid rule: col/row/cols/rows are the only position/size truth.
//            FenceManager never stores or reasons about pixels.
// ============================================================================
#include "pch.h"
#include "FenceData.h"
#include "FileOps.h"
#include <shellscalingapi.h>
#include "FenceWindow.h"
#include "GridSystem.h"
#include "SnapshotStore.h"
#include "IconCache.h"

// FenceManager owns all FenceWindow instances and handles:
// - Creating / destroying fences
// - Snapshot save / restore
// - Global hotkeys
class FenceManager
{
public:
    FenceManager() = default;

    bool Initialize(HINSTANCE hInst, HWND msgWnd)
    {
        m_hInst  = hInst;
        m_msgWnd = msgWnd;
        m_iconCache.SetRenderer(nullptr); // renderer set per-fence in Phase 2

        // Register global hotkey: Ctrl+Alt+S = Save, Ctrl+Alt+R = Restore
        RegisterHotKey(m_msgWnd, HOTKEY_SAVE,    MOD_CONTROL | MOD_ALT, 'S');
        RegisterHotKey(m_msgWnd, HOTKEY_RESTORE, MOD_CONTROL | MOD_ALT, 'R');
        RegisterHotKey(m_msgWnd, HOTKEY_NEW,     MOD_CONTROL | MOD_ALT, 'N');

        // Try to load last snapshot on startup
        auto snap = SnapshotStore::LoadStartup();
        if (snap) RestoreSnapshot(*snap);

        return true;
    }

    void Shutdown()
    {
        // Autosave current state before exit so next startup restores correctly
        AutoSave();

        UnregisterHotKey(m_msgWnd, HOTKEY_SAVE);
        UnregisterHotKey(m_msgWnd, HOTKEY_RESTORE);
        UnregisterHotKey(m_msgWnd, HOTKEY_NEW);
        m_fences.clear();
    }

    // ---- Message dispatch (call from WndProc of msgWnd) ----

    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_HOTKEY:
            if (wp == HOTKEY_SAVE)    { SaveSnapshot();    return true; }
            if (wp == HOTKEY_RESTORE) { LoadAndRestore();  return true; }
            if (wp == HOTKEY_NEW)     { CreateDefaultFence(); return true; }
            break;
break;

        case WM_SETTINGCHANGE:
            // Explorer broadcasts this when desktop icon size changes (Ctrl+Mousewheel).
            // Forward to all fences so they re-query and re-snap immediately.
            for (auto& f : m_fences)
                PostMessageW(f->GetHwnd(), WM_SETTINGCHANGE, wp, lp);
            return true;

        case WM_APP + 1: // Autosave triggered by fence change (drop, rename, delete icon)
            AutoSave();
            return true;

        case WM_APP + 59: // Fence moved - resolve collisions
        {
            HWND movedHwnd = reinterpret_cast<HWND>(wp);
            ResolveCollision(movedHwnd);
            AutoSave();
            return true;
        }

        case WM_APP + 2: // Restore from fence context menu
            LoadAndRestore();
            return true;

        case WM_APP + 3: // Delete fence
        {
            HWND toDelete = reinterpret_cast<HWND>(wp);
            RemoveFence(toDelete);
            return true;
        }
        }
        return false;
    }

    // ---- Fence CRUD ----

    FenceWindow* CreateFence(const FenceData& data)
    {
        auto fence = std::make_unique<FenceWindow>(m_hInst, &m_iconCache);
        fence->SetMessageWindow(m_msgWnd);

        if (!fence->Create(data, nullptr))
        {
            DebugLog(L"Failed to create fence: %s", data.label.c_str());
            return nullptr;
        }

        FenceWindow* ptr = fence.get();

        // Share renderer with icon cache (always use first fence's renderer).
        // Re-set after restore when fences are rebuilt from scratch.
        m_fences.push_back(std::move(fence));
        m_iconCache.SetRenderer(&m_fences[0]->GetRenderer_Hack());

        return ptr;
    }

    void RemoveFence(HWND hwnd)
    {
        auto it = std::find_if(m_fences.begin(), m_fences.end(),
            [hwnd](const auto& f) { return f->GetHwnd() == hwnd; });
        if (it == m_fences.end()) return;

        FenceWindow* fence = it->get();
        const FenceData& data = fence->GetData();

        // Move all stored icons back to Desktop before deleting
        std::wstring desktop = FileOps::GetDesktopPath();
        if (!desktop.empty())
        {
            for (const auto& icon : data.icons)
            {
                // Only move files that live in our data folder
                std::wstring dataFolder = FileOps::GetFenceDataFolder(data.id);
                if (!dataFolder.empty())
                {
                    std::wstring srcFolder =
                        std::filesystem::path(icon.parsingName).parent_path().wstring();
                    if (_wcsicmp(srcFolder.c_str(), dataFolder.c_str()) == 0)
                        FileOps::MoveToDesktop(icon.parsingName);
                }
            }
        }

        // Delete the fence data folder (should be empty now)
        std::wstring dataFolder = FileOps::GetFenceDataFolder(data.id);
        if (!dataFolder.empty())
            RemoveDirectoryW(dataFolder.c_str());

        // Clean any orphan files left in data folder before removing fence
        fence->CleanOrphanFiles();

        // Remove the fence window
        m_fences.erase(it);

        // Autosave after structural change
        AutoSave();
    }

    // Generate a unique fence ID that doesn't clash with existing fences
    // or folders already on disk.

    // Fence occupancy in slot units (with 1-slot border on all sides)
    Occupancy GetOcc(FenceWindow* fw)
    {
        // col/row/cols/rows live in FenceData — no pixel calculation needed
        const FenceData& d = fw->GetData();
        Occupancy o{ d.col, d.row, d.cols, d.rows };
        DebugLog(L"[Occ] '%s' at=(%d,%d) size=%dx%d",
            d.id.c_str(), o.col, o.row, o.cols, o.rows);
        return o;
    }



    void ResolveCollision(HWND movedHwnd)
    {
        FenceWindow* moved = nullptr;
        for (auto& fw : m_fences)
            if (fw->GetHwnd() == movedHwnd) { moved = fw.get(); break; }
        if (!moved) return;

        Occupancy movedOcc = GetOcc(moved);

        // Check collision
        bool collision = false;
        for (auto& fw : m_fences)
        {
            if (fw->GetHwnd() == movedHwnd) continue;
            if (Collides(movedOcc, GetOcc(fw.get()))) { collision = true; break; }
        }
        if (!collision) { DebugLog(L"[Grid] no collision"); return; }

        // For resize: try shrinking cols/rows until no collision (keep position)
        if (moved->IsSizingResolved())
        {
            Occupancy cand = movedOcc;
            while (cand.cols > 1 || cand.rows > 1)
            {
                // Shrink whichever dimension causes the collision
                bool colOk = true, rowOk = true;
                Occupancy testCol = cand; testCol.cols--;
                Occupancy testRow = cand; testRow.rows--;
                for (auto& fw : m_fences)
                {
                    if (fw->GetHwnd() == movedHwnd) continue;
                    Occupancy other = GetOcc(fw.get());
                    if (cand.cols > 1 && Collides(testCol, other)) colOk = false;
                    if (cand.rows > 1 && Collides(testRow, other)) rowOk = false;
                }
                if (colOk && cand.cols > 1)      { cand.cols--; }
                else if (rowOk && cand.rows > 1) { cand.rows--; }
                else break;

                bool free = true;
                for (auto& fw : m_fences)
                {
                    if (fw->GetHwnd() == movedHwnd) continue;
                    if (Collides(cand, GetOcc(fw.get()))) { free = false; break; }
                }
                if (free)
                {
                    FenceData d = moved->GetData();
                    d.cols = cand.cols; d.rows = cand.rows;
                    moved->SetData(d);
                    DebugLog(L"[Grid] resize resolved cols=%d rows=%d", cand.cols, cand.rows);
                    return;
                }
            }
        }

        // For move: spiral search for nearest free (col,row)
        int origCol = movedOcc.col, origRow = movedOcc.row;
        for (int r = 1; r < 200; ++r)
        for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
        {
            if (abs(dx) != r && abs(dy) != r) continue;
            Occupancy cand = movedOcc;
            cand.col = origCol + dx;
            cand.row = origRow + dy;
            if (cand.col < 0 || cand.row < 0) continue;

            bool free = true;
            for (auto& fw : m_fences)
            {
                if (fw->GetHwnd() == movedHwnd) continue;
                if (Collides(cand, GetOcc(fw.get()))) { free = false; break; }
            }
            if (!free) continue;

            FenceData d = moved->GetData();
            d.col = cand.col; d.row = cand.row;
            moved->SetData(d);
            DebugLog(L"[Grid] resolved col=%d row=%d", cand.col, cand.row);
            return;
        }
    }

    std::wstring GenerateFenceId()
    {
        // Check in-memory fences and on-disk folders
        auto idExists = [&](const std::wstring& id) {
            for (auto& f : m_fences)
                if (f->GetData().id == id) return true;
            return std::filesystem::exists(
                std::filesystem::path(SnapshotStore::LiveDataDir(id)));
        };
        int n = 1;
        while (true) {
            std::wstring id = L"fence_" + std::to_wstring(n);
            if (!idExists(id)) return id;
            ++n;
        }
    }

    void CreateDefaultFence()
    {
        FenceData data;
        data.id    = GenerateFenceId();
        data.label = L"New Fence";
        data.col   = 2;   // start away from screen edge
        data.row   = 2;
        data.cols  = 3;   // 3x2 cells — reasonable default
        data.rows  = 2;
        data.alpha = 0.65f;

        // Spawn centered on primary monitor, offset per fence
        MONITORINFO mi = { sizeof(mi) };
        HMONITOR hmon = MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(hmon, &mi);
        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        float scale = dpiX / 96.0f;
        // Center fence on work area in grid cells, offset per existing fence count
        int cellPhys   = static_cast<int>(Grid_CellPx(scale) * scale);
        int fenceW     = FenceWindowW(data.cols, scale);
        int fenceH     = FenceWindowH(data.rows, scale);
        int offset     = static_cast<int>(m_fences.size()) * cellPhys;
        int centerPhysX = mi.rcWork.left + (mi.rcWork.right  - mi.rcWork.left - fenceW) / 2 + offset;
        int centerPhysY = mi.rcWork.top  + (mi.rcWork.bottom - mi.rcWork.top  - fenceH) / 2 + offset;
        data.col = PixelToCol(centerPhysX, scale);
        data.row = PixelToRow(centerPhysY, scale);
        CreateFence(data);
    }

    // ---- Snapshot ----

    void SaveSnapshot()
    {
        Snapshot snap;
        for (auto& f : m_fences)
            snap.fences.push_back(f->GetData());

        if (SnapshotStore::SaveManual(snap))
            ShowNotification(L"Snapshot saved");
        else
            ShowNotification(L"Snapshot save failed");
    }

    void AutoSave()
    {
        Snapshot snap;
        for (auto& f : m_fences)
            snap.fences.push_back(f->GetData());
        SnapshotStore::SaveAuto(snap); // silent, no notification
    }

    void LoadAndRestore()
    {
        auto snap = SnapshotStore::LoadManual();
        if (!snap)
        {
            ShowNotification(L"No snapshot found");
            return;
        }
        // Sync files from snapshot/data -> live data BEFORE restoring fences
        // so parsingName paths are valid when fence windows open.
        int restored = SnapshotStore::RestoreFiles();
        DebugLog(L"RestoreFiles: %d files restored/renamed", restored);

        RestoreSnapshot(*snap);

        wchar_t msg[64];
        if (restored > 0)
            swprintf_s(msg, L"Snapshot restored (%d file(s) recovered)", restored);
        else
            wcscpy_s(msg, L"Snapshot restored");
        ShowNotification(msg);
    }

    void RestoreSnapshot(const Snapshot& snap)
    {
        // Invalidate renderer pointer before destroying fences —
        // IconCache must not hold a dangling Renderer* after fences are cleared.
        m_iconCache.SetRenderer(nullptr);
        m_iconCache.Clear();
        m_fences.clear(); // destroy existing fences
        for (auto& fdata : snap.fences)
            CreateFence(fdata);

        // TODO Phase 4: restore free icon positions via IFolderView2
    }

private:
    static constexpr int HOTKEY_SAVE    = 1;
    static constexpr int HOTKEY_RESTORE = 2;
    static constexpr int HOTKEY_NEW     = 3;

    HINSTANCE  m_hInst  = nullptr;
    HWND       m_msgWnd = nullptr;
    IconCache  m_iconCache;

    std::vector<std::unique_ptr<FenceWindow>> m_fences;

    void ShowNotification(const wchar_t* msg)
    {
        // Tray balloon tip - Phase 2 enhancement
        // For now: just debug output
        DebugLog(L"[WinFences] %s", msg);

        // Simple tray notification if tray icon is set up
        // (See main.cpp for tray icon setup)
    }
};
