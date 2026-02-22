#pragma once
// ============================================================================
// pch.h — Precompiled header. Included by every translation unit.
//
// Contains all stable Windows/COM/D2D/Shell headers that are expensive to
// parse repeatedly. Add only headers that are used project-wide and rarely
// change. Feature-specific headers belong in their own .h files.
// ============================================================================
// Windows targeting - require Win10 1703+ for PerMonitorV2 DPI
// WIN32_LEAN_AND_MEAN, NOMINMAX, UNICODE, _UNICODE are set via vcxproj PreprocessorDefinitions
#define WINVER        0x0A00
#define _WIN32_WINNT  0x0A00
#define NTDDI_VERSION NTDDI_WIN10_RS2

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shobjidl_core.h>
#include <exdisp.h>    // IShellWindows
#include <shlguid.h>   // SID_STopLevelBrowser
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>

// COM
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// Direct2D / DirectWrite / DXGI / WIC
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite_3.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wincodec.h>

// STL
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <algorithm>
#include <optional>
#include <filesystem>
#include <fstream>
#include <cassert>

// nlohmann/json (single header)
// Download json.hpp from: https://github.com/nlohmann/json/releases
// → Assets → json.hpp → place in this folder next to pch.h
#include "json.hpp"
using json = nlohmann::json;

// Lib links (can also be done in project settings)
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowscodecs.lib")

// Convenience
// Type-safe integer max/min that avoids LONG vs int ambiguity
template<typename A, typename B>
inline int imax(A a, B b) { return static_cast<int>(a) > static_cast<int>(b) ? static_cast<int>(a) : static_cast<int>(b); }
template<typename A, typename B>
inline int imin(A a, B b) { return static_cast<int>(a) < static_cast<int>(b) ? static_cast<int>(a) : static_cast<int>(b); }

#define SAFE_RELEASE(p) if(p) { (p)->Release(); (p) = nullptr; }
#define HR(x) { HRESULT _hr = (x); assert(SUCCEEDED(_hr)); }

inline void DebugLog(const wchar_t* fmt, ...)
{
#ifdef _DEBUG
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
#endif
}
