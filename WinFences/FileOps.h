#pragma once
// ============================================================================
// FileOps.h — Physical file operations between desktop and fence folders.
//
// MoveToFence(path, fenceId)     — moves file into %APPDATA%/WinFences/data/{id}/
// CopyToFence(path, fenceId)     — copies it there, source stays put
// ShortcutToFence(path, fenceId) — creates a .lnk there, source stays put
// MoveToDesktop(path)            — moves file back to %USERPROFILE%/Desktop/
//
// Handles filename collisions by appending (1), (2), ... suffixes. A move
// pre-renames the SOURCE so the move itself is conflict-free; copy and
// shortcut never touch the source and pick a free name at the destination.
// ============================================================================
#include "pch.h"

// Which physical operation a drop performs. Left-drag always means Move;
// right-drag lets the user pick (see FenceDropTarget::AskDropAction).
enum class DropAction { Move, Copy, Shortcut };

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

    // ---- Copy / shortcut (source is never modified) ------------------------

    // First free "stem (n).ext" path inside destFolder, or empty if none found.
    // Unlike PreRenameForDest this renames the DESTINATION, because copy and
    // shortcut must leave the source file exactly where it is.
    static std::wstring MakeFreeDestPath(const std::wstring& destFolder,
                                         const std::wstring& stem,
                                         const std::wstring& ext)
    {
        for (int n = 0; n < 10000; ++n)
        {
            std::wstring cand = destFolder + L"\\" + MakeNumberedName(stem, ext, n);
            if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES)
                return cand;
        }
        return {};
    }

    static bool IsDirectory(const std::wstring& path)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    // Copy srcPath into destFolder. Directories are copied recursively.
    // Returns the final destination path, or empty on failure.
    static std::wstring CopyWithCollisionHandling(const std::wstring& srcPath,
                                                  const std::wstring& destFolder)
    {
        std::filesystem::path src(srcPath);
        bool isDir = IsDirectory(srcPath);

        // A directory has no extension to preserve — its whole name is the stem.
        std::wstring stem = isDir ? src.filename().wstring() : src.stem().wstring();
        std::wstring ext  = isDir ? std::wstring() : src.extension().wstring();

        std::wstring dest = MakeFreeDestPath(destFolder, stem, ext);
        if (dest.empty()) return {};

        if (isDir)
        {
            std::error_code ec;
            std::filesystem::copy(src, std::filesystem::path(dest),
                std::filesystem::copy_options::recursive, ec);
            if (ec)
            {
                DebugLog(L"[WinFences] Copy dir FAILED src=%s dst=%s",
                    srcPath.c_str(), dest.c_str());
                return {};
            }
        }
        else if (!CopyFileExW(srcPath.c_str(), dest.c_str(),
                              nullptr, nullptr, nullptr, 0))
        {
            DebugLog(L"[WinFences] CopyFileEx FAILED src=%s dst=%s err=%lu",
                srcPath.c_str(), dest.c_str(), GetLastError());
            return {};
        }

        SHChangeNotify(isDir ? SHCNE_MKDIR : SHCNE_CREATE,
            SHCNF_PATH | SHCNF_FLUSH, dest.c_str(), nullptr);
        return dest;
    }

    // Create a .lnk inside destFolder pointing at targetPath.
    // Dropping an existing shortcut copies it instead of nesting a link to a
    // link — this is what Explorer does too.
    static std::wstring CreateShortcutIn(const std::wstring& targetPath,
                                         const std::wstring& destFolder)
    {
        std::filesystem::path src(targetPath);
        if (_wcsicmp(src.extension().wstring().c_str(), L".lnk") == 0)
            return CopyWithCollisionHandling(targetPath, destFolder);

        std::wstring stem = IsDirectory(targetPath) ? src.filename().wstring()
                                                    : src.stem().wstring();
        std::wstring dest = MakeFreeDestPath(destFolder, stem, L".lnk");
        if (dest.empty()) return {};

        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&link))))
            return {};

        link->SetPath(targetPath.c_str());
        std::wstring workDir = src.parent_path().wstring();
        if (!workDir.empty()) link->SetWorkingDirectory(workDir.c_str());

        ComPtr<IPersistFile> persist;
        if (FAILED(link.As(&persist))) return {};
        if (FAILED(persist->Save(dest.c_str(), TRUE)))
        {
            DebugLog(L"[WinFences] Shortcut save FAILED dst=%s", dest.c_str());
            return {};
        }

        SHChangeNotify(SHCNE_CREATE, SHCNF_PATH | SHCNF_FLUSH, dest.c_str(), nullptr);
        return dest;
    }

    static std::wstring CopyToFence(const std::wstring& srcPath,
                                    const std::wstring& fenceId)
    {
        std::wstring folder = GetFenceDataFolder(fenceId);
        if (folder.empty()) return {};
        return CopyWithCollisionHandling(srcPath, folder);
    }

    static std::wstring ShortcutToFence(const std::wstring& srcPath,
                                        const std::wstring& fenceId)
    {
        std::wstring folder = GetFenceDataFolder(fenceId);
        if (folder.empty()) return {};
        return CreateShortcutIn(srcPath, folder);
    }

    // Single entry point used by the drop pipeline.
    static std::wstring ApplyToFence(const std::wstring& srcPath,
                                     const std::wstring& fenceId,
                                     DropAction action)
    {
        switch (action)
        {
        case DropAction::Copy:     return CopyToFence(srcPath, fenceId);
        case DropAction::Shortcut: return ShortcutToFence(srcPath, fenceId);
        case DropAction::Move:
        default:                   return MoveToFence(srcPath, fenceId);
        }
    }
};
