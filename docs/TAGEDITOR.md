# Tag Editor Plan

Design for writing tags back to audio files — a tag editor whose document is a
`MetadataMap`, writing through a new plugin-contract family, with a capability
probe deciding what each format can actually keep.

**Current state:** the pipeline is one-directional — file → TagLib
`PropertyMap` → `MetadataMap` (arbitrary keys, multiple values, ordered) →
`PlaylistEntry` (promoted columns + remaining map) → SQLite `entry_tag`.
TagLib 2.3.2 is linked and fully capable of writing, but it has never been
called for writing. `docs/PORTING.md:4057` explicitly notes that tag writing
was never implemented in Cog — this is greenfield, not a port.

## Guiding model

A single in-memory model: **`MetadataMap` is exactly the editor's document** —
arbitrary string keys, ordered, each key holding N string values or one byte
blob (cover art). The editor is a table over this model; the writer is a
per-format best-effort serializer. "Where possible" becomes a **capability
query** that drives the UI, not `if format ==` checks in the UI.

## Phase 1 — Writer contract (`core/`)

`PluginRegistry.hpp` has `MetadataReaderDescriptor`/`MetadataReadFn` but no
writer counterpart. Add a symmetric family:

```cpp
struct MetadataCapabilities {
    bool writable            = false;  // can tags be replaced at all
    bool arbitraryKeys       = false;  // unknown keys round-trip verbatim
    bool multiValue          = false;  // 2+ values per key survive
    bool pictures            = false;  // embedded cover art
    bool lossyRepresentation = false;  // keys/values get folded (ID3 TXX, joined values)
};

using MetadataProbeFn = MetadataCapabilities (*)(const Url&);
using MetadataWriteFn = bool (*)(const Url&, const MetadataMap&, const WriteOptions&);

struct MetadataWriterDescriptor {
    std::string_view                  name;
    Priority                          priority;
    std::span<const std::string_view> extensions; // empty = claim anything, probe decides
    MetadataWriteFn                   write;
    MetadataProbeFn                   probe;
    bool (*available)()               = nullptr;
};
```

- `registry.writeMetadata(url, map)` + `registry.metadataCapabilities(url)`
  mirror `readMetadata`; same `freeze()` buckets, same `available()` dropping.
- **Probe, don't guess:** TagLib decides saveability at runtime
  (`FileRef::file()` save support, tag type). The extension list only narrows
  candidates; `probe()` opens the file and is authoritative. That automatically
  sorts WavPack, modules, MP4 quirks, and formats TagLib reads but doesn't save
  (Shorten, the ripper-header formats).
- `WriteOptions`: `stripExisting` (replace vs merge — default replace, since
  the map is the whole truth), `id3v2Version` (2.4 for MP3/AIFF/WAV — new key
  in `settings.def`), and an error sink instead of TagLib's stderr (reuse the
  `SilentDebugListener` pattern from `TagLibReader.cpp`, but surface real
  errors to the UI).

## Phase 2 — TagLib writer (`codecs/taglib/TagLibWriter.cpp`)

- Register it in the existing `xpcog_register_taglib` (same registrar, new
  source in `codecs/taglib/CMakeLists.txt`) — the writer rides the reader's
  `XPCOG_WITH_TAGLIB` gate.
- `write`: `url.localPath()` (nullopt → fail, matching the reader's stance on
  archive/HTTP sources), `FileRef(refresh=false)`, `setProperties()` per key
  (TagLib's `PropertyMap` is case-insensitively arbitrary-keyed; on ID3v2
  exotic keys fold into TXX frames — that's a "where possible" case, and the
  probe flags it as `lossyRepresentation`), `complexProperties("PICTURE")` for
  cover add/remove, then `FileRef::save()`. TagLib's default save path is
  already temp-file-plus-rename (`FileHelper::saveNewOriginal`), so crash
  safety comes free — say so in a comment; no need to reinvent the
  `WaveformCache.cpp` pattern.
- `probe`: open with `FileRef`, check the concrete `File`/`Tag` supports save,
  then report capabilities per tag family — Vorbis comments: everything;
  ID3v2: TXX folding, per-frame multi-value quirks; MP4: closed field set with
  free-form at the edge; APE: arbitrary keys, no pictures.

## Phase 3 — Map ⇄ column round-trip (`PlaylistEntry`)

`kPromoted` (`PlaylistEntry.cpp`) strips promoted keys from `entry.metadata`
with the explicit rationale "*otherwise a writer round-trip would emit it
twice*" — the counterpart the writer needs:

```cpp
MetadataMap PlaylistEntry::fullTagMap() const;   // new
```

Re-derives canonical spellings from the columns (`title`, `artist`, `album`,
`albumartist`, `genre`, `composer`, `date`, `comment`, `tracknumber`,
`discnumber`, `unsyncedlyrics`), then merges `entry.metadata`. Decisions
encoded here:

- **Canonical aliases:** write the first alias of each group already in
  `applyMetadata` — `albumartist`, `date`, `tracknumber`, `discnumber`,
  `unsyncedlyrics`. Document that saving in XPCog normalizes away
  `album artist`, `recording_date`, `tracknum`, etc.
- **Artwork:** `albumart` in the map can be file-embedded or injected by
  `attachFolderArtwork` (sidecar — the file doesn't own it). The editor must
  distinguish; never write a folder cover into every track of an album unless
  the user explicitly asks.
- **Cue-sheet entries (`album.cue#N`):** `localPath()` yields the `.cue` *text*
  file — reject any write when the URL carries a fragment, same reasoning as
  the reader's comment.
- **ReplayGain:** stays promoted into `TrackProperties`; the editor must not
  resurrect them as tags. v1 does not write RG.

## Phase 4 — Session + commands (`uicore/`)

- `Session::editTags(TrackId, MetadataMap)` in the shape of `trashTracks`: gate
  on `localPath()`, write through the registry, `Playlist::update` the entry,
  `saveEntry`, fire `tracksUpdated`, status line `"Saved tags for %zu
  file(s)."` mirroring the trash counter.
- **Dirty check:** `MetadataMap::sameContentAs()` — its doc comment is
  literally the question "did the tags change".
- **After saving, reload rather than re-trust:** call `reloadTracks()` so the
  file becomes the source of truth again; the `PluginCache` mtime+size key
  invalidates on its own. The loop closes for free.
- **Undo policy:** `AppCommands.hpp` says structural edits are undoable,
  per-entry flags are not. A file-content edit is a third thing. v1 decision:
  **no undo for tag save** — undoing a disk write is a lie once another program
  has touched the file. State that beside the existing policy comment.
- New `CommandId PlaylistEditTags`, action `edit-tags`, rows in
  `playlistMenuLayout()` and the Edit menu. Enablement follows the GTK
  MainWindow pattern: all selected entries have `localPath()`, no fragments,
  and `probe().writable`. Note: `tests/uicore/test_commands.cpp` hardcodes
  `allCommands().size()` — bump it.

## Phase 5 — Editor UI

The model lives in `uicore/` as a toolkit-free `TagEditorModel` (ordered rows,
promoted section vs. free section, per-row capability notes); both frontends
just render it.

- **Promoted section:** artist, album artist, album, title, genre, composer,
  date, track/disc, comment, lyrics — edits here also drive library grouping
  and sorting, since they are the columns.
- **Free-form section:** a key/value table over the rest of the map, straight
  from `MetadataMap` iteration order. **Arbitrary field names:** free-text key
  column — commit through `MetadataMap::normalizeKey`, display the user's
  casing. **Value counts:** each row is a value list (add/remove/reorder); the
  multi-value control disables or flags "this format will join these" per
  `probe()`.
- **Cover section:** show embedded artwork, remove, add from a file; only when
  `capabilities.pictures`.
- **Badges, not blocks:** per-row hints when folding applies — "not native to
  ID3 — will be stored as TXX", "this format keeps only the first value".
  Informational; saving still proceeds.
- **Multi-selection:** batch mode applies only the promoted fields the user
  actually changed to N files, leaving their other tags alone (merge, not
  replace).
- **GTK:** new dialog built on the `app-gtk/src/Dialogs.hpp` helper shapes,
  rows in a `GtkListBox`; **WinUI:** built in code like the other dialogs
  (`MainWindow::dialog()`, `ItemsRepeater` for free rows). All strings through
  `tr()`/`XPCOG_TRANSLATE()`, then `python tools/extract-messages.py`.

## Phase 6 — Tests

New Catch2 namespace `[tags]`, fixtures beside `tests/core/test_scanner.cpp`
via `TestShell.hpp::haveTool()` (silent skips — **read the skip count**):

- Round-trips: write with the TagLib writer → `readMetadata` → compare maps,
  including a custom key (`XPCOG_CUSTOM=...`), a two-value key, and cover
  add/remove — per format: FLAC, Ogg Opus, MP3 (ID3v2.4, TXX fold asserted),
  MP4, Wav.
- `test_library.cpp`: `fullTagMap()` — aliases collapse to canonical
  spellings, promoted keys never emitted twice, bytes entries survive.
- `test_session.cpp`: `editTags` then reload reaches every copy of the track
  (reuse the existing case's shape).
- `test_commands.cpp` count + menu-row resolution; dialog smoke cases in
  `xpcog-gtk-tests` and `xpcog-winui-tests`.
- Manual E2E: `xpcog-cli info <file>` before and after a save.

## Explicitly out of scope for v1

- **REST tag writes** — `REST.md` already rules filesystem writes by network
  peers out of scope. Keeping tag editing in `Session` rather than
  `IPlayerControl` also avoids forcing overrides in `AppPlayerControl`,
  `CliPlayerControl`, and `FakePlayerControl` for now.
- ReplayGain writing; `.lrc` sidecar writing (the tag always wins, and editing
  `unsyncedlyrics` changes lyrics precedence — document that in FEATURES.md);
  file watchers (external edits stay on manual "Reload Info"); archive members
  and HTTP streams (no local file, rejected); formats the probe reports
  unwritable (the menu item just disables).

## Ship notes

- **MINOR bump** in the same commit as the core code — `CMakeLists.txt`
  `project(VERSION)` and `vcpkg.json` `version`, per CLAUDE.md. Applied in
  Phase 1: 4.0.2 → 4.1.0, with the contract in `core/`. (AUR packaging files
  move in their own `[skip ci]` commits at release time, not here.)
- Docs: a FEATURES.md section (writability is a function of TagLib 2.3.2's
  save support as the probe sees it — don't hardcode more than examples in the
  prose); a PORTING.md note that this is new work, not port work, plus a *Where
  to pick up next* line for RG writing, `.lrc` saving, and an `xpcog-cli tag`
  command.
- Sequencing: Phases 1–3 are fully verifiable headless (`linux-headless`
  preset + `xpcog-cli`) before either frontend is touched; the UI tracks behind
  a frozen `TagEditorModel` API and can go GTK-first, WinUI second.
