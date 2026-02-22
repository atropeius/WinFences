#pragma once
// ============================================================================
// SnapshotStore.h — JSON persistence for all fence state.
//
// Saves/loads a Snapshot (vector<FenceData>) to/from:
//   %APPDATA%/WinFences/autosave.json
//
// JSON schema per fence:
//   { "id", "label", "col", "row", "cols", "rows", "alpha",
//     "icons": [ { "parsingName", "displayName" }, ... ] }
//
// Uses nlohmann/json (json.hpp). All strings are UTF-8 in JSON,
// converted to/from wstring via WideToUtf8 / Utf8ToWide.
// ============================================================================
#include "pch.h"
#include "FenceData.h"

// ── Snapshot type ────────────────────────────────────────────────────────────
struct Snapshot
{
    std::vector<FenceData> fences;
};

// ── Serialization ─────────────────────────────────────────────────────────────
// FenceData fields: id, label, col, row, cols, rows, icons[]{parsingName, displayName}

inline std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

inline json SnapshotToJson(const Snapshot& snap)
{
    json j = json::array();
    for (const auto& f : snap.fences)
    {
        json icons = json::array();
        for (const auto& ic : f.icons)
            icons.push_back({
                {"parsingName", WideToUtf8(ic.parsingName)},
                {"displayName", WideToUtf8(ic.displayName)}
            });
        j.push_back({
            {"id",    WideToUtf8(f.id)},
            {"label", WideToUtf8(f.label)},
            {"col",   f.col},  {"row",  f.row},
            {"cols",  f.cols}, {"rows", f.rows},
            {"icons", icons}
        });
    }
    return j;
}

inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

inline Snapshot SnapshotFromJson(const json& j)
{
    Snapshot snap;
    for (const auto& jf : j)
    {
        FenceData f;
        f.id    = Utf8ToWide(jf.value("id",    ""));
        f.label = Utf8ToWide(jf.value("label", ""));
        f.col   = jf.value("col",  0);
        f.row   = jf.value("row",  0);
        f.cols  = std::max(1, jf.value("cols", 1));
        f.rows  = std::max(1, jf.value("rows", 1));
        if (jf.contains("icons"))
            for (const auto& ji : jf["icons"])
            {
                FenceIconEntry e;
                e.parsingName = Utf8ToWide(ji.value("parsingName", ""));
                e.displayName = Utf8ToWide(ji.value("displayName", ""));
                e.path        = e.parsingName;
                f.icons.push_back(std::move(e));
            }
        snap.fences.push_back(std::move(f));
    }
    return snap;
}


// ---------------------------------------------------------------------------
// SnapshotStore
// ---------------------------------------------------------------------------
// Directory layout:
//
//   %APPDATA%/WinFences/
//     autosave.json              <- written on every change, JSON only
//     snapshot/                  <- manual snapshot package
//       snapshot.json            <- fence layout + icon parsingNames
//       data/
//         {fenceId}/
//           file.lnk             <- physical copies of fence files
//     snapshot.backup/           <- copy of previous snapshot before restore
//       snapshot.json
//       data/...
//
// Autosave is JSON-only (fast, no file copies).
// Manual snapshot is a full package (JSON + file copies).
// Restore syncs files back using content comparison:
//   - same name + same content -> skip
//   - same name + different content -> restore as "name (1).ext"
//   - missing -> copy back
// ---------------------------------------------------------------------------

class SnapshotStore
{
public:
    // ---- Paths ----
    static std::filesystem::path BaseDir()
    {
        wchar_t appData[MAX_PATH];
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
        return std::filesystem::path(appData) / L"WinFences";
    }

    static std::filesystem::path AutosavePath()   { return BaseDir() / L"autosave.json"; }
    static std::filesystem::path SnapshotDir()    { return BaseDir() / L"snapshot"; }
    static std::filesystem::path SnapshotJson()   { return SnapshotDir() / L"snapshot.json"; }
    static std::filesystem::path SnapshotDataDir(){ return SnapshotDir() / L"data"; }

    // Live data folder for a fence (where files actually live during use)
    static std::filesystem::path LiveDataDir(const std::wstring& fenceId)
    {
        return BaseDir() / L"data" / fenceId;
    }

    // ---- Autosave (JSON only, silent) ----
    static bool SaveAuto(const Snapshot& snapshot)
    {
        return WriteJson(snapshot, AutosavePath());
    }

    // ---- Manual Snapshot (JSON + file copies) ----
    static bool SaveManual(const Snapshot& snapshot)
    {
        try
        {
            auto snapDir = SnapshotDir();
            std::filesystem::create_directories(snapDir);

            // Write JSON
            if (!WriteJson(snapshot, SnapshotJson())) return false;

            // Copy data files: live data -> snapshot/data/
            auto liveBase = BaseDir() / L"data";
            auto snapData = SnapshotDataDir();

            // Clean old snapshot data first
            if (std::filesystem::exists(snapData))
                std::filesystem::remove_all(snapData);
            std::filesystem::create_directories(snapData);

            // Copy each fence's data folder
            if (std::filesystem::exists(liveBase))
            {
                for (auto& entry : std::filesystem::directory_iterator(liveBase))
                {
                    if (!entry.is_directory()) continue;
                    auto dest = snapData / entry.path().filename();
                    std::filesystem::create_directories(dest);
                    for (auto& file : std::filesystem::directory_iterator(entry.path()))
                    {
                        if (!file.is_regular_file()) continue;
                        std::filesystem::copy_file(file.path(), dest / file.path().filename(),
                            std::filesystem::copy_options::overwrite_existing);
                    }
                }
            }

            DebugLog(L"Manual snapshot saved to %s", snapDir.c_str());
            return true;
        }
        catch (const std::exception& e)
        {
            DebugLog(L"SaveManual failed: %S", e.what());
            return false;
        }
    }

    // ---- Load ----
    static std::optional<Snapshot> LoadStartup()
    {
        auto snap = LoadJson(AutosavePath());
        if (snap) { DebugLog(L"Startup: loaded autosave.json"); return snap; }
        snap = LoadJson(SnapshotJson());
        if (snap) { DebugLog(L"Startup: loaded snapshot/snapshot.json"); return snap; }
        return std::nullopt;
    }

    static std::optional<Snapshot> LoadManual()
    {
        return LoadJson(SnapshotJson());
    }

    // ---- Restore files (sync snapshot/data -> live data) ----
    // Call this BEFORE RestoreSnapshot() so files are in place when fences open.
    // Returns number of files restored/renamed.
    static int RestoreFiles()
    {
        int count = 0;
        auto snapData = SnapshotDataDir();
        if (!std::filesystem::exists(snapData)) return 0;

        try
        {
            for (auto& fenceDir : std::filesystem::directory_iterator(snapData))
            {
                if (!fenceDir.is_directory()) continue;
                std::wstring fenceId = fenceDir.path().filename().wstring();
                auto liveDir = LiveDataDir(fenceId);
                std::filesystem::create_directories(liveDir);

                for (auto& snapFile : std::filesystem::directory_iterator(fenceDir.path()))
                {
                    if (!snapFile.is_regular_file()) continue;
                    auto dest = liveDir / snapFile.path().filename();

                    if (!std::filesystem::exists(dest))
                    {
                        // File missing -> copy directly
                        std::filesystem::copy_file(snapFile.path(), dest);
                        DebugLog(L"Restore: copied %s", dest.c_str());
                        ++count;
                    }
                    else if (!FilesIdentical(snapFile.path(), dest))
                    {
                        // Same name, different content -> restore as "name (1).ext"
                        auto numbered = FindFreeName(liveDir,
                            snapFile.path().stem().wstring(),
                            snapFile.path().extension().wstring());
                        std::filesystem::copy_file(snapFile.path(), numbered);
                        DebugLog(L"Restore: renamed copy -> %s", numbered.c_str());
                        ++count;
                    }
                    // else: identical -> skip
                }
            }
        }
        catch (const std::exception& e)
        {
            DebugLog(L"RestoreFiles failed: %S", e.what());
        }

        return count;
    }



private:
    // Compare two files byte-by-byte (size check first for speed)
    static bool FilesIdentical(const std::filesystem::path& a,
                                const std::filesystem::path& b)
    {
        auto sa = std::filesystem::file_size(a);
        auto sb = std::filesystem::file_size(b);
        if (sa != sb) return false;
        if (sa == 0)  return true;

        std::ifstream fa(a, std::ios::binary);
        std::ifstream fb(b, std::ios::binary);
        if (!fa || !fb) return false;

        constexpr size_t BUF = 65536;
        std::vector<char> ba(BUF), bb(BUF);
        while (fa && fb)
        {
            fa.read(ba.data(), BUF);
            fb.read(bb.data(), BUF);
            auto ra = fa.gcount(), rb = fb.gcount();
            if (ra != rb) return false;
            if (ra == 0) break;
            if (std::memcmp(ba.data(), bb.data(), static_cast<size_t>(ra)) != 0)
                return false;
        }
        return true;
    }

    // Find "stem (n).ext" that doesn't exist in dir
    static std::filesystem::path FindFreeName(
        const std::filesystem::path& dir,
        const std::wstring& stem,
        const std::wstring& ext)
    {
        int n = 1;
        while (true)
        {
            auto name = stem + L" (" + std::to_wstring(n) + L")" + ext;
            auto p = dir / name;
            if (!std::filesystem::exists(p)) return p;
            ++n;
        }
    }

    static bool WriteJson(const Snapshot& snapshot, const std::filesystem::path& path)
    {
        try
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream f(path, std::ios::out | std::ios::trunc);
            if (!f.is_open()) return false;
            f << SnapshotToJson(snapshot).dump(2);
            return true;
        }
        catch (const std::exception& e)
        {
            DebugLog(L"WriteJson failed: %S", e.what());
            return false;
        }
    }

    static std::optional<Snapshot> LoadJson(const std::filesystem::path& path)
    {
        try
        {
            if (!std::filesystem::exists(path)) return std::nullopt;
            std::ifstream f(path);
            if (!f.is_open()) return std::nullopt;
            json j;
            f >> j;
            return SnapshotFromJson(j);
        }
        catch (const std::exception& e)
        {
            DebugLog(L"LoadJson failed (%s): %S", path.c_str(), e.what());
            return std::nullopt;
        }
    }
};
