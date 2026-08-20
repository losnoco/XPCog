# Moving the interface from Qt to wxWidgets

`docs/PORTING.md` is the record of getting Cog off macOS and into Qt. This is the
record of getting XPCog off Qt, which is a much smaller job than that one and for
entirely different reasons.

## Why

Not because Qt was a mistake. It was the right choice for M0–M5 and it is why
there is an application at all. Three things made it worth replacing once the
application existed:

**Qt is the only dependency that does not come from vcpkg.** Everything else in
this project — sqlite3, soxr, taglib, ffmpeg, libopenmpt, all forty-odd codec
libraries — is a line in `vcpkg.json` and arrives with the toolchain file. Qt is a
separate installer, a separate version matrix, an `XPCOG_QT_ROOT` environment
variable, a documented trap about `msvc2022_64` versus `mingw_64`, and a
`jurplel/install-qt-action` step in CI. That is a lot of surface for one
dependency, and all of it exists because Qt is not a vcpkg port.

**Qt has to be deployed.** Windows has no rpath, so a freshly built `XPCog.exe`
cannot start from Explorer until `windeployqt` has copied the runtime and its
plugins beside it. That is a build step nobody remembers until the binary fails to
launch, and it is the single largest thing in the payload.

**Qt paints its own controls.** wxWidgets wraps the platform's. For a player
that is meant to feel like a native application on three operating systems rather
than like one application on three operating systems, that is the right trade.

What is explicitly *not* a reason: licensing. XPCog is GPL-2.0-or-later and Qt's
LGPLv3 was compatible with that. wx's LGPL-with-exception is simpler, but nothing
was blocked.

## What the move is not

It is not a rewrite of the player. `xpcog-core` and `xpcog-codecs` — 57,000 lines,
the engine, the registry, the library, the playlist, every decoder — contain no Qt
and are not touched. That was the point of the Qt-free rule, and this is the
first time it has had to pay for itself. It did: the entire Qt surface is 43 files
in `app/` and 10 in `platform/`.

Nor is it a redesign. The window keeps its layout, its menus, its docks and its
behaviour. Two contracts are carried across deliberately unchanged, because they
are already toolkit-neutral and re-deriving them would only introduce differences
nobody asked for:

- `ActionRegistry`'s `ActionId` enum — 37 values, and effectively the whole
  command surface of the application.
- `PlaybackController`'s nine commands and six notifications.

## Structural decisions

### The Qt-free rule becomes a Qt-free repository

`cmake/CheckNoQt.cmake` has been failing the build on any `#include <Q…>` under
`core/` since M0. At the end of this port it stops being scoped to core and starts
scanning everything but `vendor/`. That makes the completion criterion mechanical
rather than a matter of opinion: the port is done when that target passes over the
whole tree.

### `xpcog::Signal` replaces Qt's signals, and the thread hops become visible

Core already carries a small RAII signal mechanism in
`core/include/xpcog/core/Signal.hpp`, written so core could notify without
depending on Qt. It is now the application's notification substrate too.

The consequence is worth stating plainly, because it is the one place where the
new arrangement is *less* forgiving than the old one. Qt's queued connections were
doing invisible thread marshalling: `AudioEngine::Delegate` fires on the feeder
thread, SMTC callbacks arrive on a WinRT thread, MPRIS on the D-Bus thread, and
`connect()` quietly made all three safe. `xpcog::Signal::publish()` is synchronous
and does nothing of the kind. Every one of those hops is now an explicit
`CallAfter`, at the point where it happens. That is more code and it is better
code: the places where a thread boundary is crossed are the places where the
crossing is written down.

### `platform/` links no toolkit at all

`platform/` was allowed to use Qt — that was its whole reason to exist, so core
would not have to. It ends this port linking neither Qt nor wx: Win32, C++/WinRT,
CoreFoundation, MediaPlayer.framework and GDBus, which is what the directory was
always supposed to be.

That was the point of the layer, and it took a toolkit swap to notice it was only
half true. This directory existed so core would not carry Qt, and carried Qt
itself — so "the interface is replaceable" was a claim nothing had tested.

This document said `wxBase` for a while, on the reasoning that re-implementing an
INI parser, a registry wrapper and three path conventions was more code in the
layer where bugs are hardest to test. What changed the answer was reading
`settings.def`: **every key is a flat ASCII identifier with no separator in it**,
so there are no groups to model and no escaping scheme to design. That turns
`wxConfig`'s whole value proposition into about seventy lines per platform, and
seventy lines beat a dependency in the one place that must stay substitutable.
The promise is now written into `SettingsStore.hpp`, because it is the assumption
holding the decision up.

Four public headers leaked `QString`, `QStringList`, `QUrl`, `QImage` and
`QObject`. They become `std::string`, `std::vector<std::string>`, `xpcog::Url`,
`std::vector<std::byte>` and `xpcog::Signal`. Where the OS callbacks need to reach
the GUI thread, `MediaIntegration::create()` takes an injected dispatcher rather
than reaching for a toolkit's event loop itself.

The artwork change is a straight simplification. `NowPlayingInfo::artwork` was a
decoded `QImage`, and all three backends re-encoded it: Windows went
`QImage → QBuffer → PNG → IStream`, macOS went `QImage → QBuffer → NSData`, MPRIS
wrote a temporary file. `Library::artwork()` returns the original encoded blob and
always did. Handing each backend those bytes deletes a decode-and-re-encode round
trip from all three.

### Every `wxString` is built from UTF-8 explicitly, or it is a bug

`QString::fromStdString` is UTF-8 in Qt 6. `wxString(const char*)` is **not** — on
Windows it decodes using the current 8-bit locale. There are 294 `QString` uses to
translate, most of them wrapping tag text straight out of a `PlaylistEntry`, so the
mechanical translation produces mojibake on every non-ASCII tag on Windows — and
passes CI, whose fixtures are ASCII.

The rule, and it is grep-able: **a `wxString` is constructed from a string literal
or from `wxString::FromUTF8`, never from a `std::string` or a `const char*` holding
data.** Two helpers land before any window is written:

```cpp
inline wxString    toWx(std::string_view s)  { return wxString::FromUTF8(s.data(), s.size()); }
inline std::string toUtf8(const wxString& s) { return s.utf8_string(); }
```

The test that catches it is a round trip of `Sigur Rós / Ágætis byrjun` — the string the
single-instance test already uses — through the playlist, the window title and the
settings store. It will pass on macOS and Linux while failing on Windows, which is
exactly why it has to exist.

### Settings and the library must land on the bytes that are already there

Two paths are not free to change, because a working installation already has files
at them. Measured rather than assumed, on a machine with a real library:

| | Qt today | must stay |
|---|---|---|
| Settings | `QSettings` with org `LoSnoCo`, app `XPCog` | `HKCU\Software\LoSnoCo\XPCog` |
| Library | `QStandardPaths::AppDataLocation` | `%APPDATA%\LoSnoCo\XPCog\library.db` |

`wxRegConfig` reaches the first exactly, given `SetVendorName`/`SetAppName`. The
second it does *not*: `wxStandardPaths::GetUserDataDir()` omits the vendor segment
by default, so it would answer `%APPDATA%\XPCog` and silently start an empty
library beside the real one. `platform/` links no toolkit anyway, so
`libraryDatabasePath()` is hand-rolled per OS — `SHGetKnownFolderPath` on Windows,
`$XDG_DATA_HOME` on Linux, `~/Library/Application Support` on macOS — with the
vendor segment written in.

Worth noting that the comment in `QSettingsStore.cpp` claimed `%APPDATA%/XPCog`
and was wrong about that segment. Reading the disk is what caught it.

### Resources are compiled in, by a generator rather than by the toolkit

`qt_add_resources()` has no wx equivalent — wx has XRC, which is a UI layout
format, not a blob store. `cmake/XPCogResources.cmake` and its script-mode
generator `cmake/EmbedResources.cmake` produce one `const unsigned char[]` per
file plus a lookup keyed on the path relative to the embedding directory, so call
sites keep asking for `lucide/play.svg` exactly as they asked for
`:/icons/lucide/play.svg`.

Compiled in rather than read from disk for the same reason Qt's resources were:
these files are part of the program, not configuration. An icon that can go
missing is an icon that degrades silently — which is precisely the failure
`AppIcon`'s test exists to catch.

## What has to be written by hand

wxWidgets is a smaller library than Qt and five things have no equivalent in it.
None is large; one is risky.

| | Replacing | Size |
|---|---|---|
| Undo stack | `QUndoStack` / `QUndoCommand` | ~120 lines |
| Playlist view model | `QAbstractTableModel` + `QSortFilterProxyModel` | folds two classes into one |
| Resource embedding | `qt_add_resources()` | done — see above |
| **MPRIS on GDBus** | `QDBusAbstractAdaptor` | ~420 lines |
| Taskbar overlay badge | `QImage` + `QPainter` → `HICON` | ~80 lines |

The MPRIS rewrite is the one to be careful with, and the choice of D-Bus binding
decides how careful. `QDBusAbstractAdaptor` turns a `Q_OBJECT` into a D-Bus
interface by introspecting its metaobject, and nothing else offers that -- so the
introspection XML, the `org.freedesktop.DBus.Properties` dispatch, the
`PropertiesChanged` emission and the `MediaPlayer2` / `MediaPlayer2.Player` method
tables are hand-written whichever binding is used.

**GDBus, from `gio-2.0`.** Three candidates were weighed and the first two lose
badly:

- *libdbus-1* is the low-level reference implementation. It works, it is on every
  desktop, and it makes you own main-loop integration yourself -- which means
  either a thread pumping the connection and a marshalling hop back, or hooking
  its watch/timeout callbacks into whatever loop wx is running. 600–700 lines.
- *sdbus-c++* is in the baseline after all, contrary to a first reading. It is
  still wrong: vcpkg's port depends on `libsystemd`, which is a full systemd source
  build -- meson, gperf, libcap, libmount, libxcrypt, lz4, zstd, liblzma. That is
  an absurd dependency for eleven methods.
- *GDBus* wins on the one property that matters here. wxGTK's event loop **is** a
  `GMainLoop` on the default main context, so an object registered from the GUI
  thread has its method calls delivered on the GUI thread -- exactly the guarantee
  QtDBus was quietly providing. No pump thread, no dispatcher, no `CallAfter`.
  GLib is already linked, because vcpkg's wxwidgets depends on gtk3 on Linux.
  `g_dbus_connection_register_object()` takes the introspection XML and one vtable;
  `g_dbus_connection_emit_signal()` does `PropertiesChanged` in a single call.

So this comes out at roughly **420 lines against today's 488** -- the one hand-written
item that is smaller than what it replaces.

What does not change is the risk. The failure mode is a desktop panel that shows
nothing and reports nothing, the file is compiled only by CI and only on Linux, and
there is no test. All four of the non-obvious spec facts recorded in
`LinuxMediaIntegration.cpp` -- properties do not self-announce, times are signed
microseconds, `mpris:trackid` is an object path, `Position` must not be announced --
carry across verbatim, as does the `Seek`-is-relative / `SetPosition`-is-absolute
resolution. Copy those comments; do not re-derive them.


## Deliberate regressions

Stated here rather than discovered later.

- **Run-time style switching goes away.** `Appearance.cpp` offered `windows11`,
  `windowsvista` and `Fusion` because Qt draws its own controls and can therefore
  draw them several ways. wx uses the platform's, which is the point, and has no
  `QStyleFactory`. The `WidgetStyle` setting becomes dead rather than silently
  broken. Dark mode comes from `wxSystemAppearance` and `MSWEnableDarkMode()`
  instead.
- **The file tree stops watching the filesystem.** `QFileSystemModel` watched;
  `wxGenericDirCtrl` does not. Live refresh needs `wxFileSystemWatcher` wired up
  explicitly, or the tree is stale until re-expanded.
- **The SC-55 panel gains a per-frame copy.** The emulator's `lcd_buffer_t` was
  wrapped zero-copy as a `QImage::Format_RGBX8888`. wx wants separate RGB and
  alpha planes, so the panel repacks RGBX to RGB24 each redraw.
- **The single-instance handover has no test any more.** The Qt suite had one that
  claimed a name, connected to it and checked the arguments arrived. Its wx
  equivalent needs a running event loop for `wxTCPServer` to accept on, and this
  suite deliberately has no application object. The payload's encoding is covered;
  the handshake is a hand check.
- **`wxDataViewCtrl` is not `QTableView`.** Expect drift in header sorting,
  in-place editing and selection rendering.
- **On Linux, vcpkg builds GTK3 from source.** wxwidgets' vcpkg port depends on
  gtk3, which pulls glib, pango, cairo, gdk-pixbuf, harfbuzz, at-spi2-core,
  libepoxy and wayland. On Windows and macOS the wx build is small and
  self-contained; on Linux it is not, and `x64-linux` is a static triplet by
  default, which parts of that stack have never been happy about. This is the one
  item that could derail the port rather than merely cost time, and it is why
  `cmake/XPCogWx.cmake` has a module-mode fallback from the first commit: a Linux
  developer, and possibly CI, should use the distribution's wxGTK rather than
  building GTK twice. **Unresolved, and unresolvable from this machine.**

## Staging

**Every step ends with a building tree and a green `ctest`.** No step is allowed to
leave the application unbuildable, and that is a deliberate constraint rather than
a nicety: a 7,500-line interface that has never once compiled produces a wall of
errors with no bisect point in it.

What makes that affordable is that the two applications coexist. `xpcog-app` keeps
building on Qt while `xpcog-app-wx` grows beside it, so every step has a working
player to compare against and the wx one can be launched half-finished. Qt is
deleted in one piece at the end, once the new window has been driven by hand and
found to match. The cost is that `XPCOG_QT_ROOT` stays required until step 8; the
wx target never needs it.

| | Step | State |
|---|---|---|
| ✅ | **1** — Branch, `vcpkg.json` `gui` feature, `XPCogWx.cmake`, resource embedding | done |
| ✅ | **2** — De-Qt `platform/` entirely: four headers, four backends, the settings store, the dispatcher. The Qt app adapts in place and keeps running | done |
| ✅ | **3** — Core gains what the UI will need: undo stack, `SerialExecutor`, the playlist view model. `PlaylistCommands` and `ScanTask` move down with them, and four test files move into the headless suite | done |
| ✅ | **4** — `xpcog-app-wx` exists: `wxApp`, an empty frame, embedded resources, `LucideIcon`, `AppIcon`, the UTF-8 helpers | done |
| ✅ | **5** — The main window: menus on command IDs, `wxDataViewCtrl`, transport, `SeekBar`, `FileTree`, drag and drop. **The wx build plays audio here** | done |
| ✅ | **6** — Preferences, info, about, open-URL, equaliser, mini player, tray presence, single instance | done |
| ✅ | **7** — The painted widgets: spectrum, SC-55 panel | done |
| ✅ | **8** — Delete the Qt application and `XPCogQt.cmake`, widen the layering check to the whole tree, update CI and README | done |

Steps 2 and 3 are worth doing even if the port were abandoned: they move roughly
600 lines of test out of the GUI suite and into the headless one, and they delete
`PlaylistModel`'s `rows_` bridge, which exists only because two frameworks
disagreed about when to speak.

### Step 1 — scaffolding (done)

`wxwidgets` joins `vcpkg.json` under a new `gui` feature rather than as a plain
dependency, so `macos-headless` — which builds no application — does not drag the
toolkit, and on Linux GTK, into a configuration that links neither. A vcpkg
feature cannot be subtracted by an inheriting preset, so `macos-headless` restates
the feature list without it.

`cmake/XPCogWx.cmake` tries CONFIG mode first, which is what vcpkg's own `usage`
file recommends and what its port installs, then falls back to CMake's bundled
`FindwxWidgets` for a wx that came from the system package manager. A Linux
developer with `libwxgtk3.2-dev` already installed should not have to build GTK
through vcpkg to compile this. Either path produces one target, `XPCog::wx`.

`adv` is deliberately absent from the component list: wxWidgets 3.3 merged wxadv
into core, and asking for it by name fails on exactly the version this targets.

The resource generator wraps hex into lines *before* expanding it into `0x..,`
literals, so the line-breaking pass runs over the hex rather than over the
five-times-larger text it becomes. For the SC-55 background — 776 KiB, the one
genuinely large resource here — that is the difference between a slow configure
step and a 1.6-second one.

### Step 2 — the platform layer (done)

Two replacements deleted code rather than adding it. `NowPlayingInfo::artwork`
became the encoded bytes and took a decode-and-re-encode round trip out of all
three backends. `registerFileAssociations()` took a `QStringList` that the caller
built by copying `PluginRegistry::allExtensions()`, which already answers with
exactly the `std::span<const std::string>` the new signature takes.

SMTC got shorter for an unrelated reason worth recording, because it is the shape
of a bug rather than a translation. It binds to an HWND — `GetForCurrentView()`
needs a CoreWindow only UWP has — and it was constructed before `MainWindow` had a
native window. So it went looking for one, found none, and carried a
deferred-acquisition-and-retry path for the rest of its life. Building it *after*
the window and handing the handle in deleted the retry, the second job the
`unavailable_` latch was doing, and the replay-what-was-already-playing block.

Three things are hand-written where a library used to be:

- **The taskbar badge**, four `QPainter` calls, is now sixty lines of coverage
  sampling — sixteen samples a pixel on a 32x32 image, twice a track. That is the
  honest price of this directory linking no drawing library.
- **The settings stores**, one per platform. macOS talks to CFPreferences because
  the requirement is to read a real plist written by a different program, and Cog
  stores `repeat` as an integer rather than a string. Plain C++: CoreFoundation is
  a C API.
- **MPRIS on GDBus**, 420 lines against 488.

Two paths were measured rather than assumed, and one contradicted a comment in the
tree: `QSettingsStore.cpp` said the library lived at `%APPDATA%/XPCog`, and
`QStandardPaths::AppDataLocation` includes the organisation segment, so it is
actually `%APPDATA%/LoSnoCo/XPCog`. Landing one directory over would have shown a
factory-fresh player to someone with a 9 MB library and said nothing about why.

The Linux file store writes a `[General]` header and skips section lines on the way
in, which is the whole of the difference from what QSettings wrote for flat scalar
keys, so an existing configuration is inherited rather than discarded. It also
escapes newlines — `UserDefaultURLsKey` holds the URL history newline-separated, and
a raw write would turn one setting into fifteen unparseable lines.

**What this step could not verify.** 569 cases green and no warnings, identical to
the commit before it — but that covers the Windows backend and the shared code, and
nothing else. macOS and Linux compile only in CI, and no test anywhere can say
whether a desktop panel likes the MPRIS output or whether the SMTC card still shows
artwork. Those are hand checks:

1. **Windows** — play a track with embedded art: the SMTC card shows title, artist,
   album and the artwork, and dragging its scrubber seeks. Media keys work. The
   taskbar badge appears on play, flips on pause, clears on stop. A large folder
   scan fills the taskbar progress bar. `XPCog --register`, then Open-with lists
   XPCog with its icon; `--unregister` removes it.
2. **Linux** — `playerctl status`, `metadata`, `play-pause`, `next`,
   `position 30`; a panel media widget shows title, artist and album art with a
   working seek bar; the panel's volume slider and XPCog's agree in both
   directions; `playerctl raise` and `quit`.
3. **macOS** — Now Playing in Control Centre with artwork, media keys, and the
   scrubber seeking.
4. **All three** — settings and the playlist survive a restart, which is the check
   that the store landed on the same bytes the Qt build used.

### Step 8 — the purge (done)

`app/wx/` became `app/src/`, `xpcog-wxcore` became `xpcog-appcore`, and the Qt
application, `cmake/XPCogQt.cmake`, `XPCOG_QT_ROOT`, the `deploy` target, the Qt
matrix in CI and the `install-qt-action` step are all gone. `app/CMakeLists.txt`
kept its macOS `actool` block, its `Info.plist.in` and its Windows version
resource verbatim: none of that ever knew what drew the windows.

**`cmake/CheckNoQt.cmake` became `cmake/CheckNoToolkit.cmake`**, and grew from one
rule scoped to `core/` into three scoped to the tree:

1. Nothing anywhere includes Qt.
2. `core/` and `codecs/` include no UI toolkit at all.
3. `platform/`'s **public headers** name none either -- the implementations may,
   and do not.

The old check had become tautological: with Qt gone it could only ever pass. The
new one was tested by breaking it, twice, with a `#include <QString>` and then a
`#include <wx/window.h>` dropped into `core/src/`. Both fail the build with the
right message.

### The docks are docks

An earlier pass replaced the four `QDockWidget`s with panels shown and hidden in a
box sizer, on the reasoning that wxAUI draws its own captions and none of the four
needed tearing off. That was wrong, and worth recording as wrong: a dock that
cannot be moved, re-tabbed or floated is not a dock, it is a panel with a menu
item. `QDockWidget` gave all three and the port has to as well.

So `wxAuiManager` manages the frame, and the spectrum, equaliser, info panel and
SC-55 panel are panes: draggable to any edge, tabbable with each other, floatable
into real windows, closable by their own button. The file browser stays a splitter
pane rather than a dock, which is the Qt build's own choice and Cog's shape — its
tree is a fixed part of the window.

Two things are deliberately not dockable. The transport is a pane so the manager
owns the whole frame, but it is fixed and has no close button: hiding the only
play button leaves a window with no way to start playback and no obvious way back,
which is why the Qt build removed the transport from its own context menu too. And
`restoreState()` forces the transport visible whatever the saved layout says, so a
perspective written by a build that allowed closing it cannot strand anyone.

`SavePerspective()` and `LoadPerspective()` are what `saveState()` and
`restoreState()` were, with the same trap carried across: the layout is saved only
while the window is on screen. Close-to-tray makes "save a layout with nothing
visible" the normal path, and a layout captured then is not the one the listener
arranged. Pane *names* are load-bearing in the same way object names were —
renaming one silently discards that pane's saved position.

### Twelve things that only show up when you run it

The suite was green through all of this and had nothing to say about any of them.
Each was found by running the application.

**The transport buttons did nothing.** A menu item and an accelerator raise
`wxEVT_MENU`; a `wxBitmapButton` raises `wxEVT_BUTTON`. The commands were bound on
the first, so the buttons posted an event that reached the end of the chain
unhandled -- no error, no warning, nothing. Every command now binds both, which
keeps the rule that a command has one handler whatever surface posts it.

**The panels would not appear.** `togglePanel()` called `Show()` and then
`Layout()` on the frame. The frame has no sizer: its single child does, and that
child owns the panels. So the panel really was shown, at zero height, and the View
menu looked inert.

**Play never became Pause.** `EVT_UPDATE_UI` relabels the menu item from state
every idle, and that is genuinely better than the Qt build's refresh call -- but a
`wxUpdateUIEvent` carries a label and an enabled state and nothing else. There is
no bitmap on it. The button had to be told separately, which is now
`refreshTransportIcons()`: one function that re-strokes for the palette *and*
picks the glyph from state, so the ordering hazard the Qt build had -- an icon
refresh putting "play" back over a running track -- cannot come back.

**Opening the executable started a second player.** `SingleInstance` was written,
compiled, covered by a test for its payload encoding, and never constructed. The
whole mechanism was dead code for two commits.

That is the failure worth naming, because the test did not and could not catch it:
a class can be complete, correct and covered while being unreachable. The
assertions were about what `encode()` and `decode()` do, and both would still pass
with the file deleted from the application.

**Window geometry did not persist**, because nothing saved it. The perspective and
the splitter sash were written; the frame's own rectangle never was. It is tracked
as it changes rather than read at save time -- a maximised window reports the
maximised rectangle and wx offers no way to ask what it would restore to -- and it
is only restored if some display still contains it. A rectangle saved on a monitor
that is no longer attached would put the window where nobody can reach it.

**The transport stopped filling the width.** `Resizable(false)` is `Fixed()`, and
`framemanager.cpp` sets a fixed pane's proportion to 0: it gets exactly its best
size inside the dock and the rest of the dock stays empty. Fixed *vertically* and
stretched horizontally is two flags, not one — `DockFixed(true)` locks the dock's
thickness while the pane keeps its proportion. `MaxSize` was the first guess and is
worth recording as wrong: wxAUI reads it only for floating panes and when saving a
perspective, never for a docked one.

**The equaliser and the album art were cut off by the pane's width.** The same
shape of mistake twice: sizing for a fixed dimension inside a container whose other
dimension is now the user's to drag. Thirty-two columns have a natural width a
narrow pane does not have, so the equaliser scrolls horizontally and takes its
pane height from its own best size. The cover fits whichever constraint binds
first and rescales on resize, from the original rather than from the last scaled
copy.

**Windows Firewall asked about a music player.** `SingleInstance` used
`<wx/sckipc.h>`, which is TCP on loopback everywhere, and the comment above the
include dismissed DDE as "a Windows-only mechanism this has no reason to want".
Backwards: `<wx/ipc.h>` picks DDE on Windows — which opens no socket at all — and
Unix domain sockets elsewhere. The binary now imports no Winsock.

That immediately surfaced the next one, which the assertion said outright:
`wxDDEServer::Execute()` carries text and nothing else, and the payload was going
as raw bytes with `wxIPC_PRIVATE`. It compiles on every platform and asserts on
Windows the first time a second launch hands anything over. It goes as UTF-8 text
now, and arrives at `OnExec`, which `wxConnectionBase` forwards text formats to —
so one override serves both transports.

**It crashed on exit, and two guesses at why were wrong before it was measured.**
`SpectrumPanel::onShow` was running on a window already being destroyed: tearing
the frame down hides its children, which sends `wxEVT_SHOW` to a panel whose C++
object is going away. An access violation inside a window procedure surfaces as
`STATUS_FATAL_USER_CALLBACK_EXCEPTION` — exit code `0xC000041D`, no message, no
dialog, nothing.

What found it was a vectored exception handler logging the fault code, the address
and a DbgHelp symbol lookup, which named the function and line on the first run.
That scaffolding is gone; `XPCogApp::OnAssertFailure` stays, because this is a
`WIN32_EXECUTABLE` with no console and an assertion raised during shutdown has no
event loop left to show a dialog on. It aborted silently before, which is why the
first two attempts were guesses.

The handler was deleted rather than guarded. `Sc55Panel` had the same pattern and
the same latent crash, and both were redundant: every path that shows or hides
those panes already calls `setActive()`, because each knows something `wxEVT_SHOW`
does not — whether anything is playing.

`~MainFrame` now calls `DestroyChildren()` explicitly, which closes a whole family
of these rather than one. A frame's children are destroyed by `~wxWindow`, *after*
its own members — so by default the spectrum panel outlives the `AudioTap` it
references, the data model outlives the `PlaylistView` it reads, and the SC-55
panel outlives the controller its position callback calls into.

**The album art scrolled instead of shrinking**, because the previous fix turned
horizontal scrolling on. That let the content stay wide and gave the reader a
scrollbar to chase it with, when what a cover should do in a narrower pane is get
smaller. With the horizontal rate at zero the virtual width *is* the client width,
so shrinking is the only thing left to do.

**HiDPI was pixelated on Windows.** Qt's platform plugin carried a DPI-awareness
manifest; nothing did afterwards, so Windows marked the process unaware and
bitmap-scaled the whole window. `wx/msw/wx.rc` supplies one, and getting it in
took three attempts worth recording:

- `wxUSE_DPI_AWARE_MANIFEST 2` alone does nothing. `wx.rc` emits **no** manifest
  unless `wxUSE_RC_MANIFEST` is also defined and non-zero.
- With both set, MSVC's linker generates its own manifest as well, and the build
  fails at the resource-to-COFF step with `CVT1100: duplicate resource` -- which
  reads like a corrupt object file rather than like two people writing the same
  thing. `/MANIFEST:NO` is the documented way to embed your own.
- The check that it worked is looking for `dpiAwareness` in the linked binary,
  not reading the header. The first attempt shipped an unaware process that looked
  exactly like the bug it was meant to fix.
