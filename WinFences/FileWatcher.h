#pragma once
// ============================================================================
// FileWatcher.h — ReadDirectoryChangesW wrapper for fence data folders.
//
// Watches %APPDATA%/WinFences/data/{fenceId}/ for file additions/removals.
// Posts WM_APP+60 to the fence message window when changes are detected,
// triggering a re-sync of the fence icon list.
//
// Runs on a dedicated background thread per fence.
// ============================================================================
#include "pch.h"

// ---------------------------------------------------------------------------
// FileWatcher
// ---------------------------------------------------------------------------
// Watches a directory for changes using ReadDirectoryChangesW on a background
// thread. Posts WM_APP+58 to the target HWND on any file change.
// Thread-safe: only HWND and atomic flag are shared between threads.
// ---------------------------------------------------------------------------

class FileWatcher
{
public:
    ~FileWatcher() { Stop(); }

    void Start(HWND hwnd, const std::wstring& dir)
    {
        Stop(); // stop any existing watcher

        if (dir.empty()) return;

        m_hwnd = hwnd;
        m_dir  = dir;
        m_stop.store(false);

        // Create directory handle for ReadDirectoryChangesW
        m_hDir = CreateFileW(
            dir.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);

        if (m_hDir == INVALID_HANDLE_VALUE)
        {
            DebugLog(L"[FileWatcher] failed to open dir: %s err=%lu",
                dir.c_str(), GetLastError());
            return;
        }

        m_thread = std::thread(&FileWatcher::ThreadProc, this);
        DebugLog(L"[FileWatcher] started for %s", dir.c_str());
    }

    void Stop()
    {
        m_stop.store(true);

        if (m_hDir != INVALID_HANDLE_VALUE)
        {
            // Cancel pending IO to unblock ReadDirectoryChangesW
            CancelIoEx(m_hDir, nullptr);
            CloseHandle(m_hDir);
            m_hDir = INVALID_HANDLE_VALUE;
        }

        if (m_thread.joinable())
            m_thread.join();
    }

private:
    HWND              m_hwnd   = nullptr;
    std::wstring      m_dir;
    HANDLE            m_hDir   = INVALID_HANDLE_VALUE;
    std::atomic<bool> m_stop   = false;
    std::thread       m_thread;

    void ThreadProc()
    {
        // Buffer for change notifications (must be DWORD-aligned)
        alignas(DWORD) BYTE buf[4096];
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return;

        while (!m_stop.load())
        {
            ResetEvent(ov.hEvent);

            DWORD bytesReturned = 0;
            BOOL ok = ReadDirectoryChangesW(
                m_hDir,
                buf, sizeof(buf),
                FALSE, // not recursive
                FILE_NOTIFY_CHANGE_FILE_NAME | // catches delete, rename, cut
                FILE_NOTIFY_CHANGE_DIR_NAME,
                &bytesReturned,
                &ov,
                nullptr);

            if (!ok && GetLastError() != ERROR_IO_PENDING)
                break;

            // Wait for change or stop signal
            DWORD wait = WaitForSingleObject(ov.hEvent, INFINITE);
            if (m_stop.load()) break;
            if (wait != WAIT_OBJECT_0) break;

            if (!GetOverlappedResult(m_hDir, &ov, &bytesReturned, FALSE))
                break;

            if (bytesReturned == 0)
                continue; // buffer overflow - still notify

            // Parse notifications and post to UI thread
            // We don't need to parse which file - just notify fence to prune
            HWND hwnd = m_hwnd;
            if (hwnd && IsWindow(hwnd))
                PostMessageW(hwnd, WM_APP + 58, 0, 0);
        }

        CloseHandle(ov.hEvent);
        DebugLog(L"[FileWatcher] thread exited");
    }
};
