#pragma once
// ============================================================================
// DpiHelper.h — Per-monitor DPI utilities.
//
// ScaleForWindow(hwnd)  — returns dpi/96.0f for the monitor the window is on.
//                         Call this any time you need to convert between
//                         logical and physical pixels.
//
// Requires Per-Monitor DPI v2 (Windows 10 1703+, set in app.manifest).
// ============================================================================
#include "pch.h"
#include <shellscalingapi.h>
#pragma comment(lib, "shcore.lib")

// All coordinates stored internally in 96-DPI logical pixels.
// Use these helpers everywhere you touch pixels.

class DpiHelper
{
public:
    // Get scale factor for a given window (live, handles per-monitor DPI)
    static float ScaleForWindow(HWND hwnd)
    {
        UINT dpi = GetDpiForWindow(hwnd);
        return static_cast<float>(dpi) / 96.0f;
    }

    // Get scale factor for a monitor
    static float ScaleForMonitor(HMONITOR hmon)
    {
        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        return static_cast<float>(dpiX) / 96.0f;
    }

    // Logical (96dpi) -> Physical pixels for a window
    static int ToPhysical(int logical, HWND hwnd)
    {
        return static_cast<int>(logical * ScaleForWindow(hwnd));
    }

    static float ToPhysicalF(float logical, HWND hwnd)
    {
        return logical * ScaleForWindow(hwnd);
    }

    // Physical pixels -> Logical (96dpi) for a window
    static int ToLogical(int physical, HWND hwnd)
    {
        float scale = ScaleForWindow(hwnd);
        return static_cast<int>(physical / scale);
    }

    static float ToLogicalF(float physical, HWND hwnd)
    {
        return physical / ScaleForWindow(hwnd);
    }

    // Convert a RECT from physical to logical coords
    static RECT ToLogicalRect(const RECT& physical, HWND hwnd)
    {
        float s = ScaleForWindow(hwnd);
        return {
            static_cast<LONG>(physical.left   / s),
            static_cast<LONG>(physical.top    / s),
            static_cast<LONG>(physical.right  / s),
            static_cast<LONG>(physical.bottom / s)
        };
    }

    // Convert a RECT from logical to physical coords
    static RECT ToPhysicalRect(const RECT& logical, HWND hwnd)
    {
        float s = ScaleForWindow(hwnd);
        return {
            static_cast<LONG>(logical.left   * s),
            static_cast<LONG>(logical.top    * s),
            static_cast<LONG>(logical.right  * s),
            static_cast<LONG>(logical.bottom * s)
        };
    }

    // D2D1_RECT_F in physical pixels for rendering
    static D2D1_RECT_F ToD2DRect(const RECT& physical)
    {
        return D2D1::RectF(
            static_cast<float>(physical.left),
            static_cast<float>(physical.top),
            static_cast<float>(physical.right),
            static_cast<float>(physical.bottom)
        );
    }

    // Get the DPI-scaled size for icons (base 48px @ 96dpi)
    static int IconSize(HWND hwnd, int baseSize = 48)
    {
        return ToPhysical(baseSize, hwnd);
    }
};
