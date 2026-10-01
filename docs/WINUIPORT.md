# The WinUI 3 frontend

`docs/WXPORT.md` is the record of moving the interface from Qt to wxWidgets, and
`docs/GTKPORT.md` of the GTK4 frontend that replaced wx on Linux. This is the
record of the third move: the Windows player rewritten on WinUI 3, built beside
the wx one until it could do everything that one did, and then the only one.
**3.0.0 builds no wxWidgets anywhere.** `app/` is gone; `app-winui/` is the
Windows player and `app-gtk/` the Linux one.

## Why

wx on Windows wraps Win32's common controls, which was the argument for it over
Qt: native widgets rather than painted lookalikes. But the native widgets on
Windows 11 are not the common controls any more. The shell, Settings, Explorer
and Media Player are WinUI -- Mica under the window, the Fluent controls, the
title bar as part of the window's content, light and dark following the system
-- and a Win32 dialog beside them reads as an older program. wx draws what
Windows had in 2009 faithfully, and that is the problem.

It started smaller, as a question: can the Windows App SDK be built at all from
CMake and Ninja, without MSBuild and without the XAML compiler? The prototype
put one WinUI island -- the playlist -- inside the wx main window
(`XPCOG_WITH_WINUI_ISLAND`). It could, and the playlist on the island was
better than the wx one, so the question became whether WinUI could carry the
whole player. It could.

## Decisions

### No MSBuild, no XAML compiler

Microsoft ships the Windows App SDK as NuGet packages whose build logic is
MSBuild `.targets` files. `cmake/XPCogWinAppSdk.cmake` fetches the packages as
the zip archives a `.nupkg` is -- foundation, interactive experiences, WinUI,
WebView2's metadata and Win2D, not the metapackage, which depends on several
hundred megabytes of AI runtime -- and wires up by hand the three things a C++
consumer needs:

- **The projection**, generated at configure time with the *Windows SDK's own*
  `cppwinrt.exe`, so the `Windows.*` headers `platform/` already uses and the
  `Microsoft.*` ones the player uses share one `winrt/base.h`. That is why
  configuring needs a Visual Studio developer shell.
- **The bootstrapper**, `Microsoft.WindowsAppRuntime.Bootstrap.dll`, linked and
  staged beside the executable: the player is unpackaged, and finds the
  installed runtime's framework package through it at start-up
  (`app-winui/src/Runtime.cpp`).
- **Nothing else.** WinRT classes are activated by name.

There are no `.xaml` files. The interface is built in code, the way the GTK
player's painted parts are; the odd template is read from XAML text at run time
with `XamlReader`, chiefly for `{ThemeResource}` brushes, which follow a light
and dark switch where a brush looked up from code would not (`WinRT.hpp`).

### Win2D without registration

The painted panes -- spectrum, oscilloscope, waveform seek bar, the SC-55's
front panel -- draw with Win2D. Win2D is not part of the Windows App Runtime,
and an unpackaged process has no manifest that registers its classes. So the
player installs a C++/WinRT activation handler (`app-winui/src/Win2D.cpp`):
classes under `Microsoft.Graphics.Canvas.*` come from `DllGetActivationFactory`
in the DLL beside the executable, and everything else goes to the system.

### The window

Mica under everything, and the window's own surfaces transparent so it shows
through; regions that hold content sit on `LayerFillColorDefault` cards, as the
Windows 11 layering guidance has it. The title bar is WinUI's `TitleBar`
control, extended into, holding the menu bar and the playing track; the
transport is a row beneath it. The panes are fixed regions with drag handles
between them (`Sizer.cpp`) rather than wx's AUI docking.

The menus are built from uicore's command table, as the GTK and wx ones were,
so the three never disagreed about what a menu holds. Their shortcuts are
`KeyboardAccelerator`s on the window's root, because the items of a closed menu
are not in the visual tree where an accelerator has to be to hear a key.

The playlist is a virtualised `ListView` over a boxed-index vector, filled in
`ContainerContentChanging` straight from `PlaylistView`, with a header drawn by
hand: columns resize, reorder and sort, and the filter is a button at its end.

### What wx did and the player now does itself

- **The tray icon** (`Tray.cpp`): `Shell_NotifyIcon` on a hidden window of its
  own, showing uicore's `trayMenuModel()`, so its rows are the GTK tray's. Close
  to tray, the "still running" notice once ever, and the track announcement
  from the icon with the cover.
- **Single instance** (`Instance.cpp`): the Windows App SDK's `AppInstance`
  rather than wx's checker and DDE. A later launch redirects its activation --
  its command line -- to the running player.
- **The mini player** (`MiniPlayer.cpp`): a window that *is* its title bar,
  with the transport as the title bar's content. That is the shape Cog's own mini
  window has, which neither wx nor GTK could reproduce.

### Things that are no longer the player's job

- **File associations.** `XPCog --register` is gone with wx, and with it
  `platform/.../FileAssociations`. The installer writes the same keys itself,
  under the install's scope rather than always HKCU, from a list generated at
  packaging time from the codec registry (`packaging/windows/Associations.cmake`).
- **The Windows App Runtime.** Unpackaged means framework-dependent: the
  runtime has to be on the machine. The installer checks for it and downloads
  Microsoft's installer only where it is missing (`WindowsAppRuntime.ps1`);
  it is 120 MB, which is why it is fetched rather than carried.

## Layering

Unchanged, which was the point of it. `core`, `codecs`, `uicore` and
`platform`'s headers name no toolkit; both frontends sit on `uicore`. What the
WinUI player needed that the wx one had kept to itself moved down rather than
being copied: the credits table gained a WinUI group, the setting effects and
the tray menu model were already there. `cmake/CheckNoToolkit.cmake` now
refuses wx anywhere, as it refuses Qt, and WinUI (`Microsoft.UI.*`) below the
frontends and in the GTK one.

## Testing

`xpcog-winui-tests` runs the real XAML application with Catch2 on a worker
thread beside it, handing each step to the interface thread and letting the
loop turn between steps. It opens the main window, toggles every pane the View
menu shows, walks Preferences page by page, and opens the mini player and the
About box, failing on any XAML error or unhandled exception. Without the
runtime it skips; under `CI` it fails instead, and the workflow installs the
runtime first. GitHub's Windows runner gives it a desktop.

The cases from the wx suite that were never about wx moved to the uicore suite:
the catalogue checks, the substituter and the crash reporter's consent rules.

## Deliberate differences from the wx player

- **No docking.** Panes are fixed regions with drag handles, as in the GTK
  player; View → Dock Panes is not offered.
- **Segoe Fluent Icons** instead of the Lucide set wx stroked from SVG.
- **The tray icon answers a single click**, as Windows 11's own do, where wx
  wanted a double-click.
- **The crash-reporting question is asked on the full window** before the mini
  player is restored, since a window one title bar tall cannot hold a dialog.
- **The window's size and pane layout start from defaults once** after the
  upgrade: they are kept under new keys. Settings, the library, the playlist
  and the Last.fm session carry over untouched.
- **More memory.** Measured on debug builds with the same playlist: about
  180 MB working set against wx's 120 MB. XAML and the composition engine cost
  that, and it was judged worth paying.

## Things that only show up when you run it

- **A stowed exception, `0xC000027B`, with nothing on screen**, the first time
  XAML text named a property that does not exist (`FontFeatures` on a
  TextBlock). `OnLaunched` now catches and shows the runtime's message.
- **"abort() has been called" on close**: a destructor touching XAML after the
  loop had ended threw, and a throw from a destructor is `std::terminate`.
  Nothing in a destructor here calls XAML unguarded.
- **"Invalid Xml syntax" from the manifest**, twice: `--` inside an XML
  comment. The manifest's comment now says not to.
- **"Ordinal 380 could not be located"**: `LoadIconMetric` is comctl32 v6
  only, and the manifest had not asked for Common Controls 6.
- **A centred menu bar and a seek bar a few pixels wide**: the `TitleBar`
  template aligns its content by a theme resource and pins it Left in compact
  mode (microsoft-ui-xaml #11181). Content is sized to the template's column
  outright (`fitTitleBarContent()` in `Chrome.cpp`).
- **Repeat ticks that vanished**: Repeat and Shuffle are adjacent radio rows
  divided only by a separator, and became one group.
- **crashpad's handler could not load `z.dll`**: vcpkg builds its tools in
  release, so the handler wants the release zlib beside it
  (`xpcog_stage_crashpad()`).
- **The test binary passed every case and then crashed**: the window's `Closed`
  handlers ran on a `MainWindow` already destroyed, and then the session's
  queued work on a destroyed session. The suite now quits as the player does,
  and destroys the player after the loop has ended.
