# WinFences

A lightweight desktop icon organizer for Windows 10/11 — Direct2D-rendered "fences"
that snap to the desktop icon grid and hold real files.

Native C++20, no runtime dependencies, single ~590 KB executable.

---

## What it does

WinFences puts translucent, resizable containers ("fences") on your desktop.
Each fence is a rectangle of desktop icon cells and holds a list of shell items —
files, folders, `.lnk` shortcuts, `.url` links, virtual shell items and Store apps.

Unlike an overlay that only *hides* icons, dropping a file into a fence **physically
moves it** into that fence's data folder under `%APPDATA%\WinFences\data\{fenceId}\`.
Dragging it back out moves it back to the desktop. The desktop stays clean because
the files really aren't there anymore. Right-drag if you would rather copy or leave
a shortcut behind.

**Features**

- Snap-to-grid placement — fences align to the same cell grid Explorer uses for
  desktop icons, and re-snap automatically when the icon size changes (Ctrl+Wheel)
- Collision resolution — moved or resized fences never overlap; a minimum gap of one
  cell is enforced
- Per-monitor DPI v2, correct across mixed-DPI multi-monitor setups
- Full OLE drag & drop, in and out, including drops from Explorer and other apps —
  right-drag opens the familiar *Copy / Move / Create shortcuts here* menu on release
- Native shell context menu on every icon (right-click behaves like the real desktop),
  extended with a Rename entry the shell cannot supply here — see *Rename* below
- Delete and F2 work on a selected icon the way they do in Explorer; Delete always
  goes to the Recycle Bin
- Shell display names and overlay badges — a shortcut shows as `Dokument` with the
  arrow, not `Dokument.lnk`
- Icons requested at the size they are drawn, so they stay sharp at any DPI
- Per-fence accent colour, picked from the standard Windows colour dialog and
  saved with the layout
- Double-click the empty desktop to hide every fence, double-click again to bring
  them back (not persisted — a restart always shows them)
- Live folder watching — files added to a fence's data folder outside the app show up
  immediately
- Autosave on every change, plus manual snapshots that also copy the files
- Runs from the system tray, no elevation required

---

## Requirements

| | |
|---|---|
| OS | Windows 10 1703 (Creators Update) or newer, x64 |
| Build | Visual Studio 2022 or 2026 with the *Desktop development with C++* workload |
| SDK | Windows 10 SDK 10.0.26100 or newer |
| Language | C++20 (`/std:c++20`) |

Third-party code: [nlohmann/json](https://github.com/nlohmann/json) is vendored as the
single header [`WinFences/json.hpp`](WinFences/json.hpp). Nothing else to fetch.

---

## Building

Open `WinFences.sln` in Visual Studio and build, or from a shell:

```bash
msbuild WinFences.sln -p:Configuration=Release -p:Platform=x64 -m
```

Output lands in `x64\Release\WinFences.exe`.

The project uses `$(DefaultPlatformToolset)`, so it builds with whatever MSVC toolset
your Visual Studio installation ships (v143 on VS2022, v145 on VS2026) without needing
to be retargeted.

Release is built with `/MT` (static CRT) and LTCG — the resulting `.exe` is standalone
and needs no redistributable.

---

## Usage

WinFences has no main window. It lives in the system tray.

**Tray menu / global hotkeys**

| Action | Hotkey |
|---|---|
| New Fence | `Ctrl+Alt+N` |
| Save Snapshot | `Ctrl+Alt+S` |
| Restore Snapshot | `Ctrl+Alt+R` |
| Exit | — |

**Fence interaction**

| Action | Result |
|---|---|
| Drag the header bar | Move the fence (snaps to grid on release) |
| Drag an edge or corner | Resize in whole cells |
| Right-click the header | Fence menu: Rename, Choose Color, Save/Restore Snapshot, Delete Fence |
| Double-click the empty desktop | Hide all fences; again to show them |
| Click an icon | Select it (shows the full name) |
| Double-click an icon | Launch the item |
| Right-click an icon | Native shell context menu, plus Rename |
| Arrow keys | Move the selection within the fence (stops at the edges) |
| `Enter` on a selected icon | Launch it |
| `F2` on a selected icon | Rename it |
| `Delete` on a selected icon | Move it to the Recycle Bin |
| Drag an icon out | Move the file back to the desktop or into another app |
| Drop files onto a fence (left button) | Move them into the fence |
| Drop files onto a fence (right button) | Menu: Copy here / Move here / Create shortcuts here / Cancel |

Deleting a fence leaves its files behind in its data folder; orphaned folders are
cleaned up by `FenceManager::CleanOrphanFiles`.

### Rename

Renaming acts on the item in the fence. For a shortcut that is the `.lnk` itself —
its target is never touched. The editor is pre-filled with the same text the fence
shows, so a shortcut is edited without its `.lnk` and the extension is put back on
save. Deletes and renames both go through `IFileOperation` with `FOF_ALLOWUNDO`, so
Ctrl+Z in Explorer undoes them.

Rename is WinFences' own entry rather than a shell command, because it has to be:
Rename is a verb of the *folder view* (`SHELLDLL_DefView`), which owns the inline
edit box, not a verb of the item. The context menu WinFences builds through
`BindToHandler(BHID_SFUIObject)` has no view behind it and genuinely contains no
`rename` verb — the same reason Refresh, Paste and Undo are missing from it.

---

## Where your data lives

Everything WinFences writes lives under one directory. There is no registry key, no
`%LOCALAPPDATA%` state and no on-disk icon cache — the icon cache is in memory only
and is rebuilt at every start.

```
%APPDATA%\WinFences\
  autosave.json          written on every change (layout only, no file copies)
  data\
    {fenceId}\           the actual files held by each fence
  snapshot\              manual snapshot — a full package
    snapshot.json          fence layout + icon parsing names
    data\{fenceId}\        physical copies of the files
  snapshot.backup\       previous snapshot, saved before a restore overwrites it
```

`{fenceId}` is `fence_1`, `fence_2`, … — assigned at creation and reused as the
folder name, so a fence's files are trivially findable. A typical install is a few
tens of kilobytes: the fences hold `.lnk` files, not the programs themselves.

Autosave is JSON only and therefore cheap. A manual snapshot (`Ctrl+Alt+S`) also copies
the files, so a restore can bring back content that was deleted afterwards. Restore
compares file contents: identical files are skipped, conflicting ones come back as
`name (1).ext`, missing ones are copied.

One fence entry in `autosave.json`:

```json
{ "id": "fence_1", "label": "Games",
  "col": 4, "row": 3, "cols": 6, "rows": 2,
  "color": 7551534,
  "icons": [ { "parsingName": "C:\\...\\data\\fence_1\\Sable.lnk",
               "displayName": "Sable" } ] }
```

`color` is a `COLORREF` (`0x00BBGGRR`). Snapshots written before per-fence colours
have no `color` key and fall back to the default blue.

---

## Architecture

Header-only by design: every module is a self-contained `.h`, and `main.cpp` is the only
translation unit (plus `pch.cpp`, which builds the precompiled header). ~4,000 lines total.

```
main.cpp              Entry point, tray icon, message-only window, message loop
 └─ FenceManager.h    Owns all fences: CRUD, snapshots, collision resolution,
                      data-folder sync, shell change notifications
     └─ FenceWindow.h One fence window: rendering, input, snap, D&D, file watching
         ├─ Renderer.h          Direct2D / DirectWrite render target wrapper
         ├─ IconCache.h         Shell icon loading, two-level WIC + D2D cache
         ├─ FenceDropTarget.h   IDropTarget — files dropped in
         ├─ FenceDragSource.h   IDropSource — icons dragged out
         ├─ FileWatcher.h       ReadDirectoryChangesW on a background thread
         ├─ FileOps.h           Physical move / copy / shortcut into a fence folder
         └─ ShellActions.h      Launch, context menu, rename, recycle — shell APIs

GridSystem.h          The single coordinate system (see below)
FenceData.h           Plain data types, no pixels
SnapshotStore.h       JSON persistence (nlohmann/json)
DpiHelper.h           Per-monitor DPI scale lookups
```

### The one rule: grid coordinates are the truth

`FenceData` stores position and size **only** as grid cell indices — `col`, `row`,
`cols`, `rows`. It contains no pixel values at all. Pixels are derived on demand
through a single conversion in [`GridSystem.h`](WinFences/GridSystem.h).

After any move or resize, `WM_EXITSIZEMOVE` converts the window's physical position back
to `col`/`row` and then re-derives the canonical pixel rectangle from it. Nothing else in
the codebase does pixel math. This is what keeps fences aligned across DPI changes,
monitor changes and icon-size changes.

The cell stride follows Explorer's own desktop metrics, queried live via `IFolderView2::
GetViewModeAndIconSize`, so a fence's grid always matches the desktop underneath it.

### Rendering

Each fence is a `WS_POPUP | WS_EX_LAYERED` window with a DWM glass backdrop, drawn with
an `ID2D1HwndRenderTarget` configured at `dpiX = dpiY = 96`. That makes one D2D unit
exactly one *physical* pixel, so every D2D coordinate and font size in the code is in
physical pixels — deliberately, to avoid a second implicit scaling layer on top of the
grid system.

The border is stroked **after** the header bar, not with the background. A 1 px stroke
is centred on its path, so half of it lies inside the rounded-rect geometry the header
bar is clipped against; stroking it first let the header paint over its inner half and
the outline disappeared along the top edge.

Icons are loaded through `IShellItemImageFactory` (with `SHIL_JUMBO` as a
fallback) and cached twice: as an `IWICBitmap` per path, shared across all fences and
surviving render-target recreation, and as an `ID2D1Bitmap` per (path, render target)
pair, because D2D bitmaps are bound to the factory that created them.

### Icon size: ask for what you draw

Both cache keys carry the requested size (`<path>|<size>`). `DrawIcon` derives that
size from the destination rectangle and rounds it **up** to the ladder Windows stores
icons at (16/20/24/32/40/48/64/96/128/256), so an icon is downscaled rather than
blown up: at 150 % DPI a 48 px slot is 72 physical px, asks for 96, and shrinks it.

Asking for a fixed 256 px instead is the trap. `SIIGBF_RESIZETOFIT` only ever shrinks —
an item whose largest icon is 48 px comes back centred at 1:1 inside an empty 256 px
canvas, which then draws as a tiny picture in a big empty frame. When that still
happens (the item has nothing at the requested size), `MeasureContentExtent` spots the
padding and reloads at the item's real size. That measurement goes through WIC, not
`GetDIBits`: GetDIBits does not return the alpha channel of these bitmaps and reports
every pixel opaque, which makes the padding undetectable.

Shell overlays — the shortcut arrow, cloud-sync badges — are **not** baked into the
cached icon. Letting the image list composite them (`INDEXTOOVERLAYMASK`) draws the
badge at ~39 % of the icon edge, far too loud at fence sizes. They are cached
separately and drawn as a second pass at `IconCache::OVERLAY_SCALE`, anchored to the
bottom-left corner. Finding the overlay index needs `SHGFI_ICON | SHGFI_OVERLAYINDEX`;
with `SHGFI_SYSICONINDEX` the overlay bits are always zero.

### Keyboard, and why it needs a hook

A fence is `WS_EX_NOACTIVATE` and is shown with `SW_SHOWNOACTIVATE`, so clicking an
icon never gives the window keyboard focus and it never receives `WM_KEYDOWN`. Arrow
keys, Enter, F2 and Delete therefore come from a low-level keyboard hook that is
installed only while an icon is selected, and that **swallows** them. Without it,
Delete over a selected fence icon reached the desktop instead and deleted the
desktop's own selection.

That hook is global, so it needs a rule for when a fence may claim a key. Gating on
the desktop being foreground does not work: clicking a `WS_EX_NOACTIVATE` window
never changes the foreground window, and a fence is usually clicked straight out of
whatever application the user was in — the desktop would never be foreground and no
key would ever arrive.

What matters instead is whether the user has gone somewhere else since selecting.
Clicking anywhere outside a fence already clears the selection, so the only other way
out is Alt+Tab, and that changes the foreground window. The hook therefore compares
the current foreground window against the one recorded when the selection was made.
Nobody's focus is stolen, and a selection left behind by an Alt+Tab stops eating keys
in the application switched to.

The desktop double-click that hides all fences is detected the same way, in
`FenceManager`: a low-level mouse hook never sees `WM_LBUTTONDBLCLK` — that message is
synthesised per window — so two button-ups inside `GetDoubleClickTime()` and the
system's click slop are matched by hand. Whether the click landed on empty desktop is
decided on the UI thread, not in the hook, because it has to ask Explorer
(`LVM_GETSELECTEDCOUNT` on the desktop `SysListView32`, via `SendMessageTimeout`).

---

## License

MIT — see [LICENSE](LICENSE).
