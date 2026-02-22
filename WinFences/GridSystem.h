#pragma once
// GridSystem.h — ONE grid, ONE coordinate system.
//
// The desktop is a square grid. Every icon slot is one cell.
// A fence is a rectangle of cells. The fence window is sized to exactly
// contain its cells, with a fixed header above and a fixed margin around.
//
// ONLY TWO THINGS EXIST:
//   col, row          — cell index on the desktop grid (integers)
//   GridToPixel(col, row, scale) — the SINGLE conversion to physical screen pixels
//
// Everything else (fence position, fence size, icon position inside fence,
// hit testing) is derived from col/row via GridToPixel. No other pixel math.

#include <algorithm>
#include "FenceData.h"

// ── Grid constants (logical, 96-DPI) ────────────────────────────────────────

inline int Grid_CellPx(float scale)  // stride: icon + label + gap
{
    int icon  = GetIconLogicalPx(scale);
    int label = std::max(12, icon / 4);
    return icon + label + 4;  // 4 = gap between cells
}
inline int Grid_IconPx(float scale)  { return GetIconLogicalPx(scale); }
inline int Grid_LabelPx(float scale) { return std::max(12, GetIconLogicalPx(scale) / 4); }
inline int Grid_GapPx()              { return 8;  }  // logical, gap between cells
inline int Grid_HeaderPx()           { return 32; }  // fence title bar, logical
inline int Grid_MarginPx()           { return 14; }  // fence border gap, logical

// ── THE one conversion ───────────────────────────────────────────────────────
// Physical pixel position of the TOP-LEFT of cell (col, row) on the desktop.

inline int GridToPixelX(int col, float scale)
{
    return static_cast<int>(col * Grid_CellPx(scale) * scale);
}
inline int GridToPixelY(int row, float scale)
{
    return static_cast<int>(row * Grid_CellPx(scale) * scale);
}

// ── Fence window — physical pixels ──────────────────────────────────────────
// Window is offset so that cell (col,row) is at GridToPixel(col,row).
// Header and margin sit above/around the cells.

inline int FenceWindowX(int col, float scale)
{
    return GridToPixelX(col, scale) - static_cast<int>(Grid_MarginPx() * scale);
}
inline int FenceWindowY(int row, float scale)
{
    return GridToPixelY(row, scale) - static_cast<int>((Grid_HeaderPx() + Grid_MarginPx()) * scale);
}
inline int FenceWindowW(int cols, float scale)
{
    return static_cast<int>((cols * Grid_CellPx(scale) - Grid_GapPx() + 2 * Grid_MarginPx()) * scale);
}
inline int FenceWindowH(int rows, float scale)
{
    return static_cast<int>((rows * Grid_CellPx(scale) - Grid_GapPx() + Grid_HeaderPx() + 2 * Grid_MarginPx()) * scale);
}

// ── Icon / label position inside fence (PHYSICAL pixels) ────────────────────
// D2D render target has dpi=96 → 1 unit = 1 physical pixel.
// All coordinates must be in physical pixels.

inline float CellX(int col, float scale)
{
    return static_cast<float>((Grid_MarginPx() + col * Grid_CellPx(scale)) * scale);
}
inline float CellY(int row, float scale)
{
    return static_cast<float>((Grid_HeaderPx() + Grid_MarginPx() + row * Grid_CellPx(scale)) * scale);
}
inline D2D1_RECT_F IconRect(int col, int row, float scale)
{
    float x = CellX(col, scale);
    float y = CellY(row, scale);
    float s = static_cast<float>(Grid_IconPx(scale) * scale);  // physical icon size
    return D2D1::RectF(x, y, x + s, y + s);
}
inline D2D1_RECT_F LabelRect(int col, int row, float scale)
{
    float x  = CellX(col, scale);
    float y  = CellY(row, scale) + static_cast<float>(Grid_IconPx(scale) * scale);
    float w  = static_cast<float>(Grid_IconPx(scale)  * scale);
    float lh = static_cast<float>(Grid_LabelPx(scale) * scale);
    return D2D1::RectF(x, y, x + w, y + lh);
}

// ── Snap: physical pixels → nearest col/row ──────────────────────────────────
// Used ONCE in WM_EXITSIZEMOVE. Result is stored in FenceData. Never used again.

inline int PixelToCol(int physWindowX, float scale)
{
    int firstIconPhys = physWindowX + static_cast<int>(Grid_MarginPx() * scale);
    int cellPhys      = static_cast<int>(Grid_CellPx(scale) * scale);
    return (firstIconPhys + cellPhys / 2) / cellPhys;
}
inline int PixelToRow(int physWindowY, float scale)
{
    int firstIconPhys = physWindowY + static_cast<int>((Grid_HeaderPx() + Grid_MarginPx()) * scale);
    int cellPhys      = static_cast<int>(Grid_CellPx(scale) * scale);
    return (firstIconPhys + cellPhys / 2) / cellPhys;
}
inline int PixelToCols(int physW, float scale)
{
    int cellPhys = static_cast<int>(Grid_CellPx(scale) * scale);
    int avail    = physW - static_cast<int>(2 * Grid_MarginPx() * scale)
                       + static_cast<int>(Grid_GapPx() * scale);
    return std::max(1, avail / cellPhys);
}
inline int PixelToRows(int physH, float scale)
{
    int cellPhys = static_cast<int>(Grid_CellPx(scale) * scale);
    int avail    = physH - static_cast<int>((Grid_HeaderPx() + 2 * Grid_MarginPx()) * scale)
                       + static_cast<int>(Grid_GapPx() * scale);
    return std::max(1, avail / cellPhys);
}

// ── Hit testing (physical client coords) ─────────────────────────────────────

inline int PixelToIconIndex(int physX, int physY, int cols, int rows, float scale)
{
    int cellPhys  = static_cast<int>(Grid_CellPx(scale)  * scale);
    int iconPhys  = static_cast<int>(Grid_IconPx(scale)   * scale);
    int slotHPhys = static_cast<int>((Grid_IconPx(scale) + Grid_LabelPx(scale)) * scale);
    int margPhys  = static_cast<int>(Grid_MarginPx()  * scale);
    int headPhys  = static_cast<int>(Grid_HeaderPx()  * scale);

    if (physY < headPhys) return -1;
    int lx = physX - margPhys;
    int ly = physY - headPhys - margPhys;
    if (lx < 0 || ly < 0) return -1;
    int col    = lx / cellPhys;
    int row    = ly / cellPhys;
    int localX = lx % cellPhys;
    int localY = ly % cellPhys;
    if (col >= cols || row >= rows)  return -1;
    if (localX > iconPhys || localY > slotHPhys) return -1;
    return row * cols + col;
}

constexpr int FENCE_MIN_GAP = 1;  // minimum empty cells between any two fences — change here only

// ── Occupancy + collision ─────────────────────────────────────────────────────

struct Occupancy { int col, row, cols, rows; };

// Two fences collide if they overlap on BOTH axes (extended by border).
// border = minimum gap required between fences on each axis independently.
// If separated by >= border on ANY axis, they are free.
inline bool Collides(const Occupancy& a, const Occupancy& b, int border = FENCE_MIN_GAP)
{
    bool sepH = (a.col + a.cols + border <= b.col) || (b.col + b.cols + border <= a.col);
    bool sepV = (a.row + a.rows + border <= b.row) || (b.row + b.rows + border <= a.row);
    return !sepH && !sepV;  // collides only if NOT separated on both axes
}
