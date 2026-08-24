# Reading a Cog installation

What a Cog library actually looks like on disk, established against a live one
(Cog 3633, macOS 27) on 2026-08-24, so that the reader can be written without
one to hand.

Written down because this is the half of `cogimport` that **expires**. A Core
Data store is not a schema anyone wrote; the only reliable way to learn what a
column means is to change something in Cog and look at what moved, and that
needs the program as well as the file. The reader itself is ordinary
cross-platform code that can be written anywhere, afterwards, from this page.

## Where it is, and why that is not what the roadmap said

```
~/Library/Application Support/Cog/DataModel.sqlite   the library
~/Library/Preferences/org.cogx.cog.plist             the settings
```

The roadmap said `Default.storedata`. It is not, and the mistake is instructive
enough to keep: Cog has **two** persistence layers in its source tree.

- `Utils/SQLiteStore.m` is hand-written and opens `Default.sqlite`. It is still
  compiled and still referenced (`PlaylistLoader.m`, `AppController.m`), behind
  a `+databaseStarted` guard.
- Core Data, through `PlaylistController.sharedPersistentContainer`, backed by
  `DataModel.xcdatamodeld` and stored in `DataModel.sqlite`.

On a current installation only `DataModel.sqlite` exists, and it is the one
being written. **Core Data is the live store.** Do not be misled by the
`metadataMigrated` default, which sounds like a store migration and is not — it
records that tags have been re-read, and is set at the end of the same function
that loads the playlist out of Core Data.

Read the store as a **copy**. It is WAL-mode and Cog may be running; the `-wal`
and `-shm` files must be copied alongside it or recent entries are missing.

## The schema

Four entities, and the pleasant surprise is that the model is **flat**. There
are no relationships — no integer foreign keys into tables named after nothing,
which is the usual reason Core Data stores are unreadable. Every join is a soft
one on a string.

```
ZPLAYLISTENTRY   the playlist, one row per entry
ZPLAYCOUNT       play counts and ratings, keyed by strings
ZALBUMARTWORK    cover art, joined to entries by ZARTHASH
ZSANDBOXTOKEN    what Cog was granted access to
Z_PRIMARYKEY     Core Data's own entity table; Z_NAME gives the real names
```

`Z_PRIMARYKEY` is worth reading first on any store: it maps `Z_ENT` to
`AlbumArtwork` / `PlayCount` / `PlaylistEntry` / `SandboxToken`, which is how to
confirm the entity numbering rather than assuming it.

### ZPLAYLISTENTRY, the columns that matter

| Column | Meaning |
|---|---|
| `ZINDEX` | **The playlist order.** Dense, from 0. Not `Z_PK`, which is insertion order and does not match. |
| `ZURLSTRING` | The entry's URL, percent-encoded, fragment included |
| `ZDELETED` | Pruned on load. So must the reader — see below |
| `ZTOTALFRAMES`, `ZSAMPLERATE`, `ZBITRATE`, `ZCHANNELS`, `ZBITSPERSAMPLE`, `ZCODEC` | Cached stream properties |
| `ZREPLAYGAIN{TRACK,ALBUM}{GAIN,PEAK}` | ReplayGain, already in dB and linear peak |
| `ZCURRENT`, `ZCURRENTPOSITION` | Which entry was playing and where it had got to |
| `ZQUEUED`, `ZQUEUEPOSITION` | The play queue. `-1` when not queued |
| `ZSHUFFLEINDEX` | The shuffle order, read back by `readShuffleListFromDataStore` |
| `ZARTHASH` | Joins to `ZALBUMARTWORK.ZARTHASH` |
| `ZMETADATABLOB` | Tags. An **NSKeyedArchiver** archive, not a plain plist — see below |
| `ZURLBOOKMARK` | A per-entry security-scoped bookmark. Empty on the store examined |
| `ZTRASHURLSTRING` | Where a file went when Cog trashed it |
| `ZDBINDEX`, `ZENTRYID` | Zero throughout the store examined. These belong to `SQLiteStore`, not to Core Data, and appear to be vestigial here |

`ZQUEUED` and `ZREMOVED` are `NULL` rather than `0` when unset, which is
Core Data's optional-Boolean encoding. Test for truth, not for presence.

### The two prunes, which are not optional

`PlaylistLoader.m` drops an entry when

```objc
pe.deLeted || !pe.urlString || ![pe.urlString length]
```

so a reader that keeps them produces a playlist Cog itself would not show. Note
`deLeted` — the capital L is Cog's, avoiding a collision with NSObject.

### The metadata blob

`PlaylistEntry.metadataBlob` is a Core Data `Transformable` whose transformer is
`MaybeSecureValueDataTransformer`. What lands in the column is a binary plist
whose contents are an `NSKeyedArchiver` archive of a flat `NSDictionary` of
strings:

```
$archiver = NSKeyedArchiver
$top      = {root -> UID}
$objects  = ["$null", "album", "genre", ..., <values>, {NS.keys, NS.objects}, {$classname}]
```

The dictionary is a **graph with UID references**, not a tree, so reading it is
more than reading a plist: resolve `$top.root` into `$objects`, then walk
`NS.keys` and `NS.objects` in step, dereferencing each UID. `PropertyList.cpp`
already parses the binary plist container; the keyed-archive layer on top is
new, and small.

Keys seen: `title`, `artist`, `album`, `albumartist`, `genre`, `track`, `disc`,
`year`, `comment`, `codec`, `cuesheet`, `unsynced lyrics`.

**It may not be worth reading at all.** XPCog's scanner reads tags from the
files, which is both more current and less work. The blob earns its keep only
for entries whose files are missing or slow to reach — a network mount, an
archive member — where it is the difference between a greyed-out row with a
title and one with a URL.

### URLs

Two things to know.

`ZURLSTRING` already matches `xpcog::Url`: a percent-encoded absolute URL, with
a `#NN` fragment for cue sheet tracks and subsongs. That is the same fragment
convention XPCog uses, so these transfer **as one URL**, not as a path plus an
index. Non-`file:` schemes appear too — a Cog playlist holds streams.

And the trap: an entry may hold a macOS **file reference URL**,

```
file:///.file/id=6571714.6571725
```

which names a file by volume and inode. Current Cog normalizes these when it
loads the playlist (`Utils/CogURLNormalization.h`, added 2026-08-13), but a
store written by an older build still contains them, and they resolve only while
the file is where it was. Resolving one needs `NSURL.filePathURL`.

## The settings

`org.cogx.cog.plist` is a binary plist, and it is **sparse**: `NSUserDefaults`
persists only what differs from what the app registered at launch. On the
installation examined it held nine real settings; there was no `eq*` key, no
`GraphicEQ*` key, no `volume`, `repeat` or `shuffle`, because all of those were
still at Cog's defaults.

That is the whole shape of the settings import:

- **A key that is present** and that XPCog knows: copy it. The names and value
  ranges are already identical, deliberately — `settings.def` kept Cog's
  spellings for exactly this.
- **A key that is absent**: do nothing. It means "Cog's default", and XPCog's
  default for the same key is the same value.
- **A key XPCog does not have** (`pitch`, `tempo`, `miniPlusMode`,
  `toolbarStyleFull`, `metadataMigrated`): ignore. Two of those are Rubber Band,
  which is not ported.
- **AppKit's own keys** (`NSWindow Frame …`, `NSToolbar Configuration …`,
  `NSSplitView Subview Frames …`, `TB Display Mode`): ignore. They describe a
  window layout that does not exist here.

So the settings half is small, and mostly a filter rather than a translation.

## Why this is macOS-only

Not a limitation, a fact about the data. Two independent reasons:

1. **`ZSANDBOXTOKEN`** holds security-scoped bookmarks. `ZPATH` beside each one
   is plain readable text, so *what* was granted needs no API — but re-acquiring
   access to it does, and only macOS can.
2. **File reference URLs** resolve by inode, through Foundation, on the volume
   that holds them.

Both are about reaching the *files*. Reading the store itself is portable
SQLite, which is why the reader below is written in `core/` and tested
everywhere, while finding a Cog installation is not.

## The fixtures

`tests/fixtures/cog/` holds a synthetic store and a synthetic plist, built by
`tools/cogimport-fixture/make-fixture.py`.

Synthetic on purpose: a real store is someone's listening history, several
hundred kilobytes of it, naming paths that exist on one machine. What a test
needs is the shape — the schema verbatim, and one row per case the reader has to
get right. They are enumerated in the generator, each with the reason it is
there, so a case nobody covered reads as an absence in that file rather than as
a row nobody noticed was missing.

Covered: an ordinary file; two cue sheet tracks sharing a file; a non-ASCII
percent-encoded path; a deleted entry; an empty and a null `ZURLSTRING`; a file
reference URL; a non-`file:` stream; an entry with album art; a queued entry; the
current entry with a position; real ReplayGain values; and two entries whose
`ZINDEX` order deliberately disagrees with their `Z_PK` order, which is the one
a reader that forgets to sort will pass by accident on a tidier store.

To rebuild them:

```sh
python3 tools/cogimport-fixture/make-fixture.py tests/fixtures/cog
```

## The reader

`core/include/xpcog/core/library/CogImport.hpp`. `readCogLibrary(path)` returns
the entries in playlist order with Cog's own prunes applied, the play counts,
the sandbox paths, and a count of everything it discarded. Portable: it is
SQLite and nothing else, and it is tested on all five CI jobs against
`tests/fixtures/cog/`.

Two decisions were taken deliberately rather than defaulted.

**The metadata blob is not read.** XPCog's scanner reads tags from the files,
which is both more current than a cache of unknown age and work that has to
happen anyway. What comes out of the store is where the music is and what order
it was in -- the two things the files cannot say for themselves. ReplayGain is
the exception and *is* taken from the store, because it is exactly what a rescan
would find again and computing it is the expensive half of a scan.

**Play counts match on the full tuple, and a field Cog left empty does not
constrain the match.** The second half is what makes the first usable. On the
store this was built against, **78 rows of 84 carry no artist and no album** --
Cog simply had not filled them in -- so requiring all four fields to be equal
would import six play counts out of eighty-four. That is not a stricter match,
it is a broken one: a field that was never recorded cannot disagree with
anything. Every field Cog *did* record must match exactly.

In practice that means filename and title, which were populated on every row
observed and which together were unique across all 84. `ZFILENAME` is the last
path component **with the fragment still attached** -- `Album.cue#01` -- which is
what keeps the tracks of one cue sheet apart, and is why `Url::fileName()` alone
is not the key: a caller has to rejoin the fragment.

Matching happens *after* a rescan, and that is forced by the first decision: the
title and artist to match on are the ones the scanner read from the file, not
ones carried out of the store.

## What is not established yet

- ~~**The settings half is not written.**~~ Done.
  `platform::propertyListToXml()` converts the file, using `CFPropertyList` on
  macOS and passing an already-XML file through everywhere else, so the
  macOS-only part is a format change of about thirty lines and the mapping stays
  in core where it can be tested on every platform.

  The mapping turned out to need no table at all: a key is imported when
  `Settings::all()` holds one by the same name. `settings.def` kept Cog's
  spellings deliberately, so the agreement already exists and writing it out
  again would only give it somewhere to drift from. Absent keys are left alone,
  since `NSUserDefaults` persists only what differs from what was registered and
  "absent" means "Cog's default", which is XPCog's default too.

  Run against a real `org.cogx.cog.plist`: 4 applied, 19 ignored, **0
  mismatched** -- the last being the number that matters, since a mismatch would
  mean the two programs disagreeing about the type of a key they share.

  The count surfaced one gap, since closed: **`GraphicEQenable` had nowhere to
  land.** Cog's equaliser has an on/off switch and XPCog's had none -- `active()`
  skips a flat chain, so 0 dB was free, but there was no way to bypass a curve
  without flattening it, which is exactly what A/B-ing one needs. XPCog now has
  the same setting under the same name, so the fixture reads 11 applied and 5
  ignored rather than 10 and 6.
- **Nothing calls the reader yet.** It produces a `CogLibrary`; turning one into
  an XPCog playlist, and finding a Cog installation to read in the first place,
  are the macOS-only half.
- **`ZSHUFFLEINDEX` and the queue** were all-zero on the one store examined, so
  their behaviour under a real shuffle has been read in Cog's source but not
  observed.
- **Only one store has been read**, by one user, on one version. The columns that
  were uniformly zero here (`ZDBINDEX`, `ZENTRYID`, `ZSHUFFLEINDEX`) are the ones
  most likely to mean something on somebody else's.
