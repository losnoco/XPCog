# The GTK4/libadwaita frontend

`docs/WXPORT.md` is the record of moving the interface from Qt to wxWidgets.
This is the record of adding a second interface on Linux, written directly
against GTK4 and libadwaita, of what the two have to share for that to be one
player rather than two -- and, at the end of it, of that interface becoming
the only one Linux has.

## Why

wxWidgets on Linux is wxGTK, and wxGTK is GTK **3**. That is the toolkit GNOME
moved past in 2020: no libadwaita, no header bars, no adaptive layouts, no
`AdwStyleManager` following the desktop's light and dark preference, and a
widget set the desktop increasingly draws as a guest. wx wraps the platform's
controls -- that was the argument for it over Qt -- and on Linux the platform
has moved out from under the wrapper.

It began beside the wx frontend rather than instead of it: wx stayed on
Windows and macOS, where it wraps Win32 and AppKit and looks like it belongs,
and both Linux players built from one tree so they could be compared. Step 12
ended that, and went further than planned: **2.0.0 builds no wx on Linux and
no macOS port at all.** wx is the Windows player's toolkit and nothing else's;
on a Mac, Cog is the player.

## Decisions

- **GTK C API, not gtkmm.** GNOME's documentation describes the C API;
  libadwaita's C++ wrapper (`libadwaitamm`) is packaged by no distribution
  this targets. The join to C++ is `app-gtk/src/Glib.hpp`: an owning
  `GObjectPtr`, a `connect<Signature>()` trampoline, a `Connection` that
  severs itself, a `Timeout`, and the dispatcher core wants.
- **Blueprint for the interface.** `app-gtk/ui/*.blp` compiles to GtkBuilder
  XML and into a GResource; `cmake/XPCogBlueprint.cmake` is the pipeline.
  The compiler checks every property against the `Adw-1` typelib, which is
  the build-time validation of the interface files -- `gtk4-builder-tool
  validate` cannot do it, since it loads GTK's types alone and rejects every
  `Adw*` object.
- **The resource registers by hand.** `glib-compile-resources` registers a
  bundle from a constructor by default, and a constructor in a static library
  is the trap `cmake/XPCogCodec.cmake` warns about: nothing references its
  object file, the linker drops it, and the window fails to build with every
  build green. That is precisely what happened on the first run.
  `--manual-register` generates a plain function and `GtkApp` calls it.
- **libadwaita 1.9 / GTK 4.22 / GLib 2.88**, compiled in as the floor through
  the `*_VERSION_MAX_ALLOWED` macros in `cmake/XPCogGtk.cmake`. That is newer
  than Ubuntu 24.04 (1.5 / 4.14); Ubuntu 26.04 is the first that has it, and
  the Linux CI jobs that build the player run there (step 12).
- **Panes follow libadwaita, not wxAUI.** Sidebars and a bottom strip, toggled
  from the View actions. No docking, no floating, no tabbing: libadwaita has
  no dock manager, and Wayland cannot position a torn-off surface anyway.
- **Header bar and a primary menu, no menu bar.** Every command is a
  `GAction` generated from the same command table the wx menus are built
  from, with the same accelerators.
- **GApplication is the single instance.** The application ID is a D-Bus name;
  a second launch hands its files to the first over `open` and exits. The wx
  frontend keeps its own socket-based `SingleInstance`; the two do not talk to
  each other.

## Layering

```
xpcog-app  (wx) ──┐
                  ├── xpcog-uicore ──┬── xpcog-platform  (per-OS; NO toolkit)
xpcog-gtk (GTK4) ─┘                  └── xpcog-codecs ──┐
                                                        ├── xpcog-core (NO toolkit)
xpcog-cli ── core + codecs ─────────────────────────────┘
```

`cmake/CheckNoToolkit.cmake` enforces it: nothing below the two frontends
includes wx, GTK, libadwaita or GLib (GLib is legitimate in
`platform/src/linux` alone), `app/` includes no GTK or GLib, and `app-gtk/` no
wx. Each rule was tested by breaking it.

Until step 12 `XPCOG_BUILD_GTK_APP` was an option independent of
`XPCOG_BUILD_APP`, so one tree built both players for comparison, and a
`linux-gtk-only` preset configured with wx off -- the proof that nothing
beneath the GTK frontend needed it. Since then `XPCOG_BUILD_APP` is the one
switch and the frontend follows the platform; every Linux tree is that proof.

## Staging

Every step ends with a building tree and a green `ctest`, for the reason
WXPORT.md gives.

| | Step | State |
|---|---|---|
| ✅ | **1** — Scaffolding: option, `XPCog::gtk`, the Blueprint pipeline, presets, layering rules, an empty `AdwApplicationWindow` | done |
| ✅ | **2** — Replay the `uicore/` extraction the ImGui experiment made, resolved against what `main` has done since | done |
| ✅ | **3** — `Session`: the composition root hoisted out of `MainFrame`, consumed by both frontends | done |
| ✅ | **4** — The GTK build plays audio: actions, accelerators, primary menu, transport | done |
| ✅ | **5** — Main window: playlist, file tree, drag and drop, persistence | done |
| ✅ | **6** — Panes: sidebars and the tools strip | done |
| ✅ | **7** — Preferences and dialogs | done |
| ✅ | **8** — Desktop: tray, notifications, close-to-tray, mini player, consent | done |
| ✅ | **9** — Painted widgets: seek bar, spectrum, oscilloscope, SC-55 | done |
| ✅ | **10** — Parity audit against the command table | done |
| ✅ | **11** — `xpcog-gtk-tests` under Xvfb | done |
| ✅ | **12** — Packaging switch and CI, and the end of wx on Linux and of macOS | done |

### Step 2 — the shared layer (done)

The Dear ImGui experiment of September 2026 had already asked "which half of
`app/` is really about a toolkit?" and answered it with `uicore/`. Its commits
survived in the object store after the branch was deleted, and seven of them
were replayed here -- `git cherry-pick -n`, the ImGui hunks dropped, the rest
resolved against fifty-three commits of drift -- with three re-derived by hand
where main had rewritten the same functions since (the listener's own Last.fm
API key, the settings' value lists). One rule from that branch is kept: de-wx
in place first, `git mv` second, never both in one commit, so each diff is
about one thing.

What `uicore/` holds: the playback controller (with a `tick()` the frontend
clocks instead of a `wxTimer`), the command tables (with `CommandId` as fixed
integers and the wx menu builders left behind in `app/src/WxMenus.cpp`), the
playlist edits, the remote control's player, the setting-effect table, the
settings' value lists, the info panel's formatters, the translation lookup
and the `.mo` image builder, and the three accounts -- Last.fm, ListenBrainz,
the remote token -- over `platform::SecretStore`. Two seams joined
`platform/` for it: the password store, reproducing wx's libsecret schema
byte for byte so a session made under one binary is a session under the
other, and `platform::Notifier` for the track announcement. The ImGui
branch's `DesktopEvents` pump and `SingleInstance` were not taken: GTK
iterates the default `GMainContext` itself, and `GApplication` is the single
instance.

The wx frontend's behaviour is unchanged, and `xpcog-uicore-tests` links
`xpcog-uicore` and nothing else -- so a widget dependency creeping into the
layer fails to link, in a tree the `linux-gtk-only` preset configured with no
wx found at all.

### Step 3 — the composition root (done)

`MainFrame` was the wx application's composition root as well as its window,
and the ImGui experiment had written that wiring a second time under
`app-imgui/` rather than share it -- the step its own staging list named as
"the real blocker" and never took. `uicore/src/Session.{hpp,cpp}` is the object
graph assembled once with the window taken out: the playlist and the library,
the playback controller, the scan queue, the scrobblers and their play clock,
the lyrics lookup, the waveform provider, the remote server and the desktop
integration. What happens lives there; what draws stays in a frontend, and
learns what happened through signals published after the session has done its
own part.

`MainFrame.cpp` went from 3,434 lines to 2,297, and it plays the same. The
half of a setting change that reaches the engine, the playlist and the
services is the session's; the widget half is `onEffectApplied`. The trash
confirmation, the Cog import's picker and its explanations stay in the window
because they ask; the acting is the session's.

`tests/uicore/test_session.cpp` drives a whole session through an offline
output paced at eight times real time, which is what the injectable output
factory on `PlaybackController` is for -- and it is why the uicore suite links
codecs now: no toolkit, still, but a session that can scan nothing and play
nothing tests nothing.

### Step 8 — the desktop (done)

The tray is `platform::TrayIcon` with nothing behind it: GTK 4 has no XEmbed
tray, so where the wx frontend falls back to `wxTaskBarIcon` this one answers
"no tray" and closing the window closes it. The rows the tray shows moved to
`uicore/src/TrayMenu.{hpp,cpp}` first, so a panel sees the same menu and
tooltip whichever binary is running. The icon is decoded from the resource's
PNGs with `GdkTexture` and unpremultiplied by hand into the byte order
`TrayIcon.hpp` insists on. Notifications are `platform::Notifier`, the
`announceTrack` signal's text verbatim; a cover goes to a file under the cache
directory because the daemon takes a path. The "still running" hint on the
first close to the tray uses `TrayHideAnnounced` -- the setting exists for
exactly this and its comment says "once, ever", which the wx frame does not
quite do (its flag is per session).

The mini player is `Adw.Window` from `ui/mini.blp`, a mode as in Cog: one of
the two windows is shown, never both, and `MainWindow::present()` raises
whichever the mode says is the player's, so MPRIS `Raise` and the tray's Show
row reach the right one. It borrows the main window's action group under the
`win` prefix, so its transport buttons are the same actions the header bar
has. The main window's actions keep working while it is hidden, which is what
a `GtkApplication` does with a window that is not visible.

The crash-reporting question is an `AdwAlertDialog` over whichever window is
on screen, with the wx dialog's wording and its rule of writing "asked" before
the answer.

### Step 9 — the painted widgets (done)

Four `GtkDrawingArea`s with cairo, and the drawing is a transcription: the wx
panels paint through `wxGraphicsContext`, which is a path API with cairo's
shape, so `SeekBar.cpp`, `Visualizers.cpp` and `Sc55View.cpp` follow their
counterparts function for function and the comments that explain the
arithmetic stay in `app/`. What differs is the plumbing. The seek bar's
drag is one `GtkGestureDrag` -- begin is the click, updates the scrub, end
the seek, and a cancelled gesture seeks nothing. The two visualisers keep
their tap cursors, their measured frame interval and their visible-and-
playing gate, with the widget's own `map`/`unmap` standing in for
`IsShownOnScreen()` so the window reports only playback. Their right-click
menus are `GtkPopoverMenu`s over a `GSimpleActionGroup` on the widget,
states read from settings when the menu opens. The SC-55 hands GDK the
emulator's buffer as it is -- `GDK_MEMORY_R8G8B8X8`, stride passed in -- and
copies only because GTK uploads lazily; the repack wx needed is gone. Its
background photograph rides in the GResource beside the interface files
(`RESOURCES` learned `served=source` for a file outside `app-gtk/`).

**The plain seek bar was later made GTK's own `GtkScale`.** The wx bar is
painted because a stock slider pages on a trough click, and GTK 4's does
not: `gtk-primary-button-warps-slider` defaults to on, so a click jumps.
The scale reports every step of a drag through `change-value` and has no
"let go", so a legacy controller in the capture phase watches the press and
release the scale's own gestures take: a drag is a scrub, the release is
the seek, and a keyboard step seeks at once. The painted area is kept for
the waveform alone, in a `GtkStack` beside the scale, and waveform mode
shows the scale until a shape is known. The transport buttons moved into
the header bar at the same time, so the row under it grows to the
waveform's height without stretching them.

The accent colour comes from `AdwStyleManager`, which reads the portal, so
the seek bar follows the desktop's choice; `platform::accentColour()`
declines on Linux and stays declined. The channel-mode tables moved to
`uicore/src/VisualizerChannels` first, so the setting a wx menu writes is
one a GTK menu reads.

### Step 10 — the audit (done)

Three lists, each checked against the frontend by the compiler or by a
script rather than by reading:

- **Commands.** `MainWindow::onCommand` switches over `CommandId` with no
  `default`, so `-Wswitch` names any command the table has and the window
  does not. All fifty have an arm; `FirstWidgetId` has one that says it is
  not a command. `Actions.cpp` installs one GAction per id from
  `commandAction()`, whose names `tests/uicore/test_commands.cpp` checks
  for presence and uniqueness.
- **Setting effects.** `onEffectApplied` is exhaustive the same way, and the
  arms it leaves empty are the ones `Session::settingChanged` acts on --
  the same split the wx frame has, with `MiniFloating` empty here because
  there is nothing to float.
- **Setting keys.** Advanced draws a row for every key `hasCuratedRow()`
  declines, so the audit is the other direction: every curated key has a
  row on a GTK page. Sixty-one of sixty-six do; the five that do not --
  `volume`, `repeat`, `shuffle`, `panelFollowMode`, `widgetStyle` -- are
  the ones `SettingChoices.cpp` curates precisely so that no page offers
  them, and the wx dialog has no row for them either.

The menus need no walk: both frontends build theirs from `menuLayout()` in
uicore, and the primary menu's regrouping is the one described under step 4.

### Step 11 — the suite that needs a screen (done)

`tests/gtk/test_gtkwidgets.cpp`, one `add_test()` under `xvfb-run` like the
wx suite and skipping the same way when there is no display. What it walks:
every `.ui` in the resource built; `window.ui` built under Spanish, through
the same `installTranslations()` the application calls, and its filter box
reading "Filtrar" while `tr("Filter")` agrees; the preferences dialog page
by page with the main context run between, over a session that plays into
an offline output; the equaliser's thirty-two scales inside the strip; the
seek bar in both modes over a synthetic shape; the spectrum and the scope
through every channel mode with their menus driven as actions; the SC-55's
explanation; the playlist model's `items-changed` arithmetic, its row-level
`changed`, and a selection surviving a sort in a real `GtkColumnView`; and
the accelerator converter against every spelling the table carries, each
one parsed by `gtk_accelerator_parse`.

Two things the suite does that the wx one cannot. GTK's own warnings are
part of the verdict: a `g_log` writer collects every warning and critical,
and a case that triggered one -- "attempt to underallocate", a builder id
that does not exist, a property that does not -- fails on it. And the
pictures come from GSK rather than ImageMagick: `XPCOG_GUI_CAPTURE=<dir>`
renders each widget through the window's own renderer to a PNG, which shows
what GTK would put on screen with no root-window grab and no Xvfb-specific
setup beyond the display itself.

The suite found two things on its first run. `gtk_widget_get_width()` is
the *content* box, inside the 12 px padding Adwaita gives a scale, and so
reads 22 for a scale whose measured minimum is 28: a measurement that
compares the two is wrong, not the widget, and the column around the scale
is what is checked now. And the visualisers' check items flipped their
*action's* state rather than the setting, so a flip while the menu had
never been opened turned a setting that was on... on. They flip the setting
now and the action follows.

### Step 12 — the Linux player, and the end of macOS (done)

Planned as a packaging switch: the GTK binary becomes what the Linux install,
the tarball, the AUR package and the Flatpak ship, and CI learns to build it.
Done as more than that, by decision: wx is no longer built on Linux at all,
and the macOS port is gone. 2.0.0, because the CMake options and presets are
public surface and both changed.

- **One switch.** `XPCOG_BUILD_APP` builds the player; `cmake/XPCogOptions.cmake`
  derives `XPCOG_BUILD_WX_APP` on Windows and `XPCOG_BUILD_GTK_APP` on Linux,
  and refuses anywhere else. The `linux-gtk*` and `linux-repo-gtk*` presets
  folded into the plain `linux-*` ones; `linux-gtk-only` is every Linux tree.
- **The binary is `XPCog`**, the name the Linux player always installed under,
  so the desktop file and an upgraded package keep working. The target stays
  `xpcog-gtk`. It stages and installs `crashpad_handler` now, which the wx
  build had done on Linux and the GTK one had not -- a GTK build sent Sentry
  messages but never a crash.
- **CI**: the matrix's Linux job and the tarball job run on `ubuntu-26.04`, the
  first Ubuntu at the floor. The system-libraries job stays on 24.04 without
  the player, because its whole value is 24.04's mix of too-old and
  good-enough libraries. The macOS job and the disk image are gone, and the
  release carries two files.
- **What went with wx on Linux**: the wxGTK half of `PlaylistColumns`, the GTK 3
  dependency it brought, the Unix-socket single instance, and
  `xpcog-gui-tests`. `xpcog-app-tests` runs on Windows only;
  `test_cogplist` moved to the uicore suite, since it tests the platform
  layer and not wx.
- **What went with macOS**: `platform/src/mac`, the AVFoundation spatializer
  and its `spatializeSurround` setting, the bundle and its document types,
  `packaging/macos` (signing, the disk image, notarisation), the overlay
  triplets and the deployment-target file, and every `__APPLE__` and
  `__WXOSX__` branch outside `vendor/`.
- **And the Cog import**, File → Import from Cog with the library and settings
  readers behind it, the platform's property-list conversion, the scan
  decorator it alone used and `Library::importPlayCount`. It existed for
  moving off a Mac. Cog's XML playlists, a playlist format rather than an
  import, still open.

## Deliberate regressions

Stated here rather than discovered later.

- **No docking, floating or tabbing of panes.** "Dock Floating Panes" is not
  offered at all (`offeredHere()` in `Accelerators.hpp`): no action, no menu
  row. It began as "Reset Panel Layout", which hid every pane, and read as a
  command that did nothing.
- **No menu bar.** A primary menu, a context menu, accelerators and a
  shortcuts dialog (`AdwShortcutsDialog`, Ctrl+?, generated from the command
  table). The primary menu's row for it first named `win.show-help-overlay`,
  which only exists once a `GtkShortcutsWindow` is set, and sat greyed out.
- **Info and Lyrics share one sidebar** and cannot be shown side by side.
- **The mini player cannot stay above other windows.** GTK 4 has no
  keep-above and Wayland would not honour one.
- **The info pane is widgets, not HTML.** No "select all, copy as one text".
- **Preferences search finds pages, not rows.** The dialog is a sidebar
  (`AdwNavigationSplitView` over an `AdwViewSwitcherSidebar`) rather than an
  `AdwPreferencesDialog`, whose header view switcher is meant for a handful of
  pages and crowded twelve into unlabelled icons. Its search, a filter on the
  sidebar, keeps a page listed while any row on it matches; the dialog it
  replaced listed the matching rows themselves. The pages are grouped into
  sections -- the player, Sound, Services, then Advanced -- rather than kept
  in the wx dialog's order.

## Things that only show up when you run it

**Quit from the desktop while the consent dialog is up aborts.** The
first-launch crash-reporting prompt is modal, and MPRIS `Quit` arriving while
it is open makes the wx frame `Close(true)` under the dialog's own nested event
loop; `DestroyChildren()` then frees the dialog out from under `ShowModal()`
and glibc says `free(): invalid size`. Found driving the wx player under Xvfb
with `busctl`, present on `main` before this branch, and left alone here: it
needs a first launch and a media key in the same second.

**Opening Preferences took the tray icon down.** The Appearance page probed
for a notification area by creating a second `platform::TrayIcon` and
asking it `isAvailable()`. Two in one process own the same bus name --
`org.kde.StatusNotifierItem-<pid>-1` is the convention, and the pid is the
same -- so the probe's destructor released the real icon's name and the
panel dropped it. The window knows whether it has a tray; the dialog asks
the window. Found in the last smoke run of step 11, from two GLib warnings
about an interface already exported.

**A widget's paintable is empty until the next frame.** `GtkWidgetPaintable`
shows the widget's last painted frame, taken from the frame clock after a
paint, so a snapshot straight after `gtk_widget_paintable_new()` is a null
render node. Run the main context a few times first. A `GtkStack` snapshots
as its transition and reads as nothing between them for the same reason.

**`Adw.Window` is 200 pixels tall however little is in it.** libadwaita
gives every `AdwWindow` a size request of 360×200 in `adw_window_init`, a
floor for windows with breakpoints. The mini player has one row and no
breakpoints, and came up with a hundred pixels of nothing under it until the
Blueprint set `height-request: -1`.

**No tray on this desktop, and the code is right.** GNOME without the
AppIndicator extension has no `org.kde.StatusNotifierWatcher` on the session
bus, and `TrayIcon::isAvailable()` honestly says so, so close-to-tray closes.
Driving the tray under Xvfb took a stand-in watcher: a few lines of Python
owning that name and accepting `RegisterStatusNotifierItem`, after which the
item's tooltip, pixmaps and dbusmenu can all be read back and its rows
activated with `busctl`.

**The window did not build, with every build green.** The GResource's
registering constructor sat in an object file of a static library that nothing
referenced, so the linker left it out and `gtk_builder_new_from_resource`
reported the resource missing. See "The resource registers by hand" above.
