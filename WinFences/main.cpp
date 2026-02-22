#include "pch.h"
// ============================================================================
// main.cpp — Application entry point and message loop.
//
// Responsibilities:
//   - Initialize COM, D2D, DWrite, WIC factories
//   - Create the hidden message window (receives shell notifications, WM_APP)
//   - Instantiate FenceManager (loads snapshot, creates fence windows)
//   - Run the main message loop
//   - Handle system tray icon and context menu
//   - Relay WM_APP+59 (snap/collision) and WM_APP+60 (file watch) to FenceManager
// ============================================================================
#include "pch.h"
#include "resource.h"
#include "FenceWindow.h"
#include "FenceManager.h"

// Tray icon ID
static constexpr UINT TRAY_ICON_ID  = 1;
static constexpr UINT WM_TRAY       = WM_APP + 100;

// Message-only window for system messages
static HWND       g_msgWnd     = nullptr;
static FenceManager g_manager;
static HINSTANCE  g_hInst      = nullptr;

// ---- Tray Icon ----

static void AddTrayIcon(HWND hwnd, HINSTANCE hInst)
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd             = hwnd;
    nid.uID              = TRAY_ICON_ID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon            = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wcscpy_s(nid.szTip, L"WinFences");
    Shell_NotifyIconW(NIM_ADD, &nid);

    // Modern tray icon version (Win10+)
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void RemoveTrayIcon(HWND hwnd)
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = hwnd;
    nid.uID  = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowTrayContextMenu(HWND hwnd)
{
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1, L"New Fence\tCtrl+Alt+N");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 2, L"Save Snapshot\tCtrl+Alt+S");
    AppendMenuW(hMenu, MF_STRING, 3, L"Restore Snapshot\tCtrl+Alt+R");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 4, L"Exit");

    // Required: set foreground window before TrackPopupMenu
    SetForegroundWindow(hwnd);

    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenuEx(hMenu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
        pt.x, pt.y, hwnd, nullptr);
    DestroyMenu(hMenu);

    switch (cmd)
    {
    case 1: g_manager.CreateDefaultFence(); break;
    case 2: g_manager.SaveSnapshot();       break;
    case 3: g_manager.LoadAndRestore();     break;
    case 4: g_manager.Shutdown(); PostQuitMessage(0); break;
    }
}

// ---- Message-only window ----

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Let manager handle its messages first
    if (g_manager.HandleMessage(msg, wp, lp))
        return 0;

    switch (msg)
    {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == NIN_KEYSELECT)
            ShowTrayContextMenu(hwnd);
        return 0;

    case WM_QUERYENDSESSION:
        // Windows is shutting down - autosave immediately while we still can
        g_manager.AutoSave();
        return TRUE; // allow shutdown to proceed

    case WM_ENDSESSION:
        if (wp) // session is actually ending (not cancelled)
        {
            g_manager.AutoSave();
            RemoveTrayIcon(hwnd);
        }
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon(hwnd);
        g_manager.Shutdown();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateMessageWindow(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = MsgWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"WinFences_MsgWnd";
    RegisterClassExW(&wc);

    // Hidden message-only window (not HWND_MESSAGE - we need WM_HOTKEY which
    // requires a real window, but we make it invisible)
    return CreateWindowExW(0, L"WinFences_MsgWnd", L"WinFences",
        0, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
}

// ---- WinMain ----

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInst;

    // ---- Mutex: single instance ----
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"WinFences_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"WinFences is already running.", L"WinFences", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // ---- DPI: Per-Monitor v2 ----
    // (Also set in manifest - this is a belt-and-suspenders fallback)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ---- COM ----
    // OleInitialize initializes COM + OLE (required for RegisterDragDrop)
    OleInitialize(nullptr);

    // ---- Register fence window class ----
    FenceWindow::RegisterClass(hInst);

    // ---- Create hidden message window ----
    g_msgWnd = CreateMessageWindow(hInst);
    if (!g_msgWnd)
    {
        MessageBoxW(nullptr, L"Failed to create message window.", L"WinFences", MB_ICONERROR);
        return 1;
    }

    // ---- Tray icon ----
    AddTrayIcon(g_msgWnd, hInst);

    // ---- Initialize manager (loads snapshot / creates fences) ----
    g_manager.Initialize(hInst, g_msgWnd);

    // ---- If no snapshot, create one demo fence ----
    // (Remove this after first real use)
    // g_manager.CreateDefaultFence();

    // ---- Message loop ----
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    OleUninitialize();
    CloseHandle(hMutex);
    return static_cast<int>(msg.wParam);
}
