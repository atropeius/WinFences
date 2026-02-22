#pragma once
// ============================================================================
// FileOps.h — Physical file move operations between desktop and fence folders.
//
// MoveToFence(path, fenceId)   — moves file into %APPDATA%/WinFences/data/{id}/
// MoveToDesktop(path)          — moves file back to %USERPROFILE%/Desktop/
// EnsureFenceDataDir(fenceId)  — creates the data folder if missing.
//
// Handles filename collisions by appending (1), (2), ... suffixes.
// ============================================================================
#include "pch.h"

// ---------------------------------------------------------------------------
// FileOps
// ---------------------------------------------------------------------------
class FileOps
{
public:
    // Returns true if this is a real filesystem path (not a virtual shell item).
    // Does NOT check if the file currently exists.
    static bool IsMovable(const std::wstring& parsingName)
    {
        if (parsingName.size() < 2) return false;
        // Virtual items start with :: or shell:
        if (parsingName[0] == L':') return false;
        if (_wcsnicmp(parsingName.c_str(), L"shell:", 6) == 0) return false;
        // Real path: starts with drive letter + colon (C:) or double-backslash (UNC)
        if (parsingName[1] == L':') return true;
        if (parsingName[0] == parsingName[1]) return true; // \\ UNC
        return false;
    }

        static std::wstring GetFenceDataFolder(const std::wstring& fenceId)
    {
        wchar_t appData[MAX_PATH];
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
            return {};
        std::wstring folder = std::wstring(appData) + L"\\WinFences\\data\\" + fenceId;
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        return folder;
    }

    static std::wstring GetDesktopPath()
    {
        wchar_t path[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, path);
        return path;
    }

    // Build "stem (n).ext" style name
    static std::wstring MakeNumberedName(
        const std::wstring& stem, const std::wstring& ext, int n)
    {
        return n == 0 ? stem + ext
                      : stem + L" (" + std::to_wstring(n) + L")" + ext;
    }

    // If srcPath's filename conflicts with an existing file in destFolder,
    // rename the source file IN-PLACE before moving it.
    // This way the move itself is always conflict-free — no Shell dialogs.
    // Returns the (possibly renamed) source path.
    static std::wstring PreRenameForDest(
        const std::wstring& srcPath,
        const std::wstring& destFolder)
    {
        std::filesystem::path src(srcPath);
        std::wstring stem      = src.stem().wstring();
        std::wstring ext       = src.extension().wstring();
        std::wstring srcFolder = src.parent_path().wstring();

        // Check if the plain name is free in destination
        std::wstring destName = MakeNumberedName(stem, ext, 0);
        if (GetFileAttributesW((destFolder + L"\\" + destName).c_str())
                == INVALID_FILE_ATTRIBUTES)
            return srcPath; // no conflict

        // Find a free numbered name
        int n = 1;
        while (GetFileAttributesW(
                   (destFolder + L"\\" + MakeNumberedName(stem, ext, n)).c_str())
               != INVALID_FILE_ATTRIBUTES)
            ++n;

        std::wstring newName    = MakeNumberedName(stem, ext, n);
        std::wstring newSrcPath = srcFolder + L"\\" + newName;

        // Rename source in its current location
        if (!MoveFileExW(srcPath.c_str(), newSrcPath.c_str(), 0))
            return srcPath; // rename failed — proceed, worst case Explorer shows dialog

        // Tell Explorer about the rename so Desktop icon updates immediately
        SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATH | SHCNF_FLUSH,
            srcPath.c_str(), newSrcPath.c_str());

        return newSrcPath;
    }

    // Move srcPath into destFolder.
    // Pre-renames source if needed so there's never a name conflict at the destination.
    // Returns final destination path, or empty on failure.
    static std::wstring MoveWithCollisionHandling(
        const std::wstring& srcPath,
        const std::wstring& destFolder)
    {
        // Guard: already in target folder
        std::wstring srcFolder = std::filesystem::path(srcPath).parent_path().wstring();
        if (_wcsicmp(srcFolder.c_str(), destFolder.c_str()) == 0)
            return srcPath;

        // Pre-rename source if destination name is taken
        std::wstring actualSrc = PreRenameForDest(srcPath, destFolder);

        // Destination is now guaranteed conflict-free
        std::wstring dest = destFolder + L"\\"
            + std::filesystem::path(actualSrc).filename().wstring();

        if (!MoveFileExW(actualSrc.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED))
        {
            wchar_t dbg[512];
            swprintf_s(dbg, L"[WinFences] MoveFileEx FAILED src=%s dst=%s err=%lu",
                actualSrc.c_str(), dest.c_str(), GetLastError());
            OutputDebugStringW(dbg);
            return {};
        }

        // Notify shell: remove from source folder immediately (no F5 needed)
        SHChangeNotify(SHCNE_DELETE,    SHCNF_PATH | SHCNF_FLUSH,
            actualSrc.c_str(), nullptr);
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH | SHCNF_FLUSH,
            srcFolder.c_str(), nullptr);

        return dest;
    }

    static std::wstring MoveToFence(const std::wstring& srcPath,
                                    const std::wstring& fenceId)
    {
        std::wstring folder = GetFenceDataFolder(fenceId);
        if (folder.empty()) return {};
        return MoveWithCollisionHandling(srcPath, folder);
    }

    static std::wstring MoveToDesktop(const std::wstring& srcPath)
    {
        std::wstring desktop = GetDesktopPath();
        if (desktop.empty()) return {};
        return MoveWithCollisionHandling(srcPath, desktop);
    }
};
