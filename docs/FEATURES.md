# What XPCog does, at length

The README says what the player does in a paragraph each; this is the rest of
each of those paragraphs, moved here whole. `docs/PORTING.md` is where the
decisions were made; this is what they amount to for someone using the player.

## Trying it without the window

```sh
xpcog-cli codecs                     # what this build can decode
xpcog-cli info   song.flac           # format, duration, ReplayGain, tags
xpcog-cli expand album.cue           # the tracks a playlist or cue sheet holds
xpcog-cli info   album.cue#3         # one track of a single-file album
xpcog-cli decode song.flac out.raw   # headerless native-endian PCM
xpcog-cli waveform --dump song.flac  # the seek bar's peak/RMS buckets
xpcog-cli play   a.flac b.m4a c.mp3  # gapless across the queue
xpcog-cli serve  a.flac b.flac       # the REST remote control, no toolkit
```

## Cue sheets

A `.cue` expands to one URL per track (`album.cue#1`, `#2`, …). Opening one decodes
the referenced audio file, seeks to that track's `INDEX 01`, and stops at the next
track's start, so each track reports its own duration and metadata and seeks
relative to itself.

Two parser bugs that corrupt real albums are fixed here rather than reproduced from
upstream:

- Upstream keeps one `artist` variable for the whole sheet and never resets it per
  track, so a single track-level `PERFORMER` mis-credits every following track.
  Track-level fields here fall back to the album value instead.
- A non-`AUDIO` `TRACK` is skipped, but upstream still lets its `INDEX` create an
  entry, so a mixed-mode disc gains a bogus track that decodes to noise.

## Formats

Dedicated decoders for FLAC, Ogg Vorbis, Opus, MP3 (minimp3), WavPack and
Musepack (libmpcdec), plus FFmpeg as the catch-all for AAC, ALAC, WMA, AC3, DTS,
TAK, TTA, APE, PCM and the MP4/MKV/ASF containers.

Beyond those: tracker modules (libopenmpt), chiptune rips (Game_Music_Emu),
console streamed audio (vgmstream), the PSF family on all eight of the emulator
cores behind it — USF, GSF, 2SF, SNSF, SSF/DSF, NCSF, PSF/PSF2 and QSF — and
Commodore 64 tunes (libsidplayfp). MIDI is its own thing again: a score rather
than a recording, so what it sounds like is a choice of synthesiser. Fourteen
extensions -- `.mid` and `.midi` among them, with HMI, XMI, Doom's MUS and
Loudness LDS each reaching their own parser in midi_processing -- render on a
SoundFont bank (SpessaSynth), an emulated Sound Blaster (Nuked OPL3, under two
different drivers), or a Roland SC-55mkII running its own firmware, if you have
the ROMs.

**A bank ships with it**, so MIDI plays on real instruments out of the box
rather than on an FM chip: `GeneralUserXG-SFeTest.sf3`, together with the `tg300b`
map that XPCog selects instead when a sequence announces itself as GS or GM2. Point `soundFontPath` at your own bank
to replace it, or drop one beside a file — `song.sf2`, or `Album/Album.sf2` for
a folder — to override it for that music alone. An RMID that carries its own
bank inside it beats all of those, since that bank is part of the music.

Archives are a *source* rather than a format,
so a `.zip` of FLAC plays without being unpacked first, and a Monkey's Audio Link
(`.apl`) is a *range* within one -- the same shape as a cue sheet track, which is
how a single-file CD rip becomes an album.

`xpcog-cli codecs` prints what a given build claims; a default one is 23 decoders
and 842 extensions.

Selection is by extension first, then MIME type, with several claimants tried in
descending priority. FFmpeg registers *below* default priority,
so a dedicated decoder always wins for formats that have one, while FFmpeg still
picks up files those decoders reject.

Every codec is checked against one asymmetric reference signal — 440 Hz in the left
channel, 660 Hz in the right, at different levels — verifying per-channel frequency
and amplitude. That catches swapped, duplicated and silent channels, which a
duration or size check would miss. Adding a codec means adding a row to that table.

`decode` output is byte-identical to `flac -d` with the WAV header stripped, which
is how the decoder is regression-tested.

## Playlists

Several, as tabs over the one list; the strip hides itself while there is only one.
New, New from Selection, Rename and Delete are in the File menu (`Ctrl+T` for a new
one), Next and Previous Playlist in the View menu, and Add to Playlist on the rows.

What is on screen and what is playing are independent: looking at another playlist
never changes what plays next, and a playing row stays in its own tab. The queue is
one across all of them, as Cog's is. A queued track from another playlist carries
playback there, and when the queue empties it goes on after that track, in that
playlist. Undo is per playlist, and deleting one is not undoable, so the player asks
first when there are tracks in it. An existing library opens as a single playlist.

## Artwork

A picture embedded in the file is the track's cover. A track that carries none
takes the folder's: `cover`, `folder`, `front`, `album` or `albumart`, as
`.jpg`, `.jpeg` or `.png`, matched without regard to case and in that order of
preference, so `Cover.JPG` and `Folder.jpg` both count and a folder holding
two candidates always resolves the same way. Nothing looser than that list --
a folder scanned for "any image" would hand back the booklet scan as the
cover.

Cog reads embedded art only, so an album ripped with a `cover.jpg` beside it
plays there with a blank Info pane; here it shows the cover, and the same file
reaches the notifications, the OS's now-playing card and the remote control's
artwork route, because all of those read the same field. A cue sheet's tracks
take the cover from the sheet's folder. The lookup lives in the scanner rather
than the window, so `xpcog-cli info` reports it too, and the folder is listed
and the file read once per scan rather than once per track. Where both exist
the embedded picture wins: it was put in that file on purpose.

## Gapless playback

When a decoder reaches end of stream the engine opens the next track immediately,
while the audio already buffered is still playing, and keeps writing into the same
ring — so a same-format handoff needs no device reconfiguration and produces no gap.
Track changes are announced when the seam becomes *audible*, not when it is
decoded.

The seam is covered by tests that run the real engine against a capturing output,
so they are deterministic and need no audio device: sample-exactness against
separately-decoded references, waveform continuity across the join, notification
ordering, and a three-track case where a per-seam off-by-one accumulates rather
than cancels. The tests were confirmed to fail when a chunk is deliberately dropped
at each seam.

A track at a **different sample rate** joins gaplessly too. The device stays at the
first track's format and later tracks are resampled into it (libsoxr),
because reconfiguring the device mid-stream cannot be seamless. The outgoing
resampler is flushed before it is reconfigured, so the few milliseconds held in its
delay line — exactly the samples that meet the seam — are not lost.

Matching rates bypass the resampler entirely, so a same-rate file is passed through
bit-exactly rather than being needlessly recomputed.

Consecutive tracks of a cue sheet share one decoder: the engine asks it to move
rather than opening the file again, which on a network share is the difference
between a seam and a pause.

Choosing a track while another plays crossfades into it, the way a seek does, when
fades are on and the new track has the same sample rate and channels. Otherwise, on a
device held exclusively, or while paused it stops and starts as before, so the device
opens for the new track.

## HDCD

HDCD codes are decoded when present, expanding the extra resolution the format
carries. Because the decoder runs on *every* 16-bit 44.1 kHz stereo lossless
stream — almost all CD-sourced material, and almost none of it actually HDCD — it
has to be bit-transparent when no codes are found. It is, and that is asserted
rather than assumed.

## Real-time audio

The audio callback reads from a lock-free SPSC ring, applies an atomic gain, and
zeroes any tail it could not fill. That is the whole callback: no lock, no
allocation, no `std::function`, no logging. A feeder thread does the decoding and
writes into the ring.

That is deliberately stricter than upstream Cog, whose callback
(`Audio/Output/OutputCoreAudio.m:877`) takes an `NSLock` and enters an
`@autoreleasepool` on the real-time thread. `xpcog-cli play` reports underruns
separately for playback and for the post-stream drain, so a genuine dropout is
never confused with the expected tail.

## Signal Inspector and meters

View > Signal Inspector (`Ctrl+Alt+I`), or a click on the status line, opens a window
walking the path from the decoder to the device: each stage with its settings, and
whether it is changing the audio, passing it on, or not in use. Under that are the
levels, the output latency, what is queued and the underruns so far. "Unchanged"
means XPCog passed the decoded samples on as they were; it cannot speak for what the
system's mixer does after that.

The VU Meter pane is the same levels on their own, left over right: RMS solid, peak
behind it, the peak held for a second and a half, and a lamp for clipping that a
click clears. Scrolling Waveform shows the last four seconds as an envelope. Both
read the audio being heard, and surround is metered as its front pair.

## Crash reporting

**Off unless you turn it on.** On
the first launch XPCog asks, once, whether it may send crash reports and usage data
to <https://cog-analytics.losno.co>, and it never asks again whatever the answer
was. Preferences → General is where it is changed afterwards, and the switch takes
effect immediately in both directions — unticking it shuts the reporter down for
the running session rather than at the next launch.

Until it is ticked, no reporter is initialised, no report database is created and
nothing leaves the machine. That is deliberately stronger than the SDK's own
opt-in mode, which starts a client and then holds events back: here there is no
client. [What is collected, and what happens to
it](https://www.iubenda.com/privacy-policy/59859310).

It is [sentry-native](https://github.com/getsentry/sentry-native) underneath,
where Cog uses the Sentry Cocoa SDK, and the keys are Cog's own —
`sentryConsented` and `sentryAskedConsent` — so an answer given in Cog on macOS
carries over rather than being asked for again. The reporter lives in
`platform/`, behind a four-function header that never exposes the SDK; see
[`platform/include/xpcog/platform/CrashReporter.hpp`](platform/include/xpcog/platform/CrashReporter.hpp).

The presets build it. `-D XPCOG_WITH_SENTRY=OFF` leaves it out entirely, which is
the default for a plain `cmake` with no preset: the port builds crashpad, and that
is a lot to hand someone who only wants a player. Such a build still shows the
switch, greyed out, saying why.

## Last.fm

Scrobbling, with the **desktop** authentication flow rather than the mobile one
Cog uses: connecting opens last.fm in a browser and XPCog never sees the
password. The session key that comes back is kept in the platform's own secret
store — Credential Manager on Windows, the Secret Service on Linux — not in
the settings. The records are laid out as `wxSecretStore` laid them out, so a
session made by an older wx build is still signed in.

Plays go on a durable queue before they are sent, so an evening spent offline
arrives the next time the machine has a network. Cog has no queue; a failed
submission there is simply gone.

The API key — the one that says *which program* is submitting, as distinct from
the listener's session — is normally built in, and the releases carry one. The
pane also takes a key and shared secret of your own, from
[last.fm/api/account/create](https://www.last.fm/api/account/create), under
*API account*: for a build made without one, or for scrobbling under an
application of your own rather than XPCog's. The pair goes in the same secret
store as the session and wins over the built-in one until removed. A session
belongs to the key that opened it, so changing the key disconnects you and you
connect again under the new one. Cog has no equivalent; its key is
`Secrets.xcconfig` or nothing.

Building with credentials of your own is in [`docs/BUILDING.md`](BUILDING.md#lastfm-credentials).

The switch itself is `enableAudioScrobbler`, which is Cog's key.

## ListenBrainz

The same plays go to ListenBrainz as well, or instead: Preferences →
ListenBrainz has its own switch, and each service takes and refuses submissions
on its own, so one being down does not hold the other up and a play Last.fm has
accepted is never sent to it twice because ListenBrainz was not answering. Two
queues on disk, one per service, both durable in the way the Last.fm one is.

Connecting is a paste rather than a browser trip. ListenBrainz identifies the
listener with a user token from [listenbrainz.org/settings](https://listenbrainz.org/settings/);
XPCog asks the server whose it is, shows the answer, and keeps the token in the
same secret store as the Last.fm session — never in the settings. There is no
application key and nothing to sign, so a build made without Last.fm credentials
scrobbles to ListenBrainz all the same.

The server's address is a setting, `listenBrainzUrl`, because more than one
server speaks the API: a ListenBrainz you run yourself, or Maloja's
compatibility endpoint, take the same requests at their own root. Both `host`
and `host/1` are accepted spellings. Every listen names XPCog and its version as
the submitting client, which is what the service asks of clients and what tells
these plays apart from the same ones sent by another program. Cog has no
ListenBrainz; this is new work on the queue Cog also does not have.

## Lyrics

The Lyrics pane shows what the file carries — the `unsyncedlyrics` tag, from
FLAC, Vorbis, Opus and ID3's USLT frame, or a `.lrc` file of the same name
beside it — and, when it carries nothing, can ask
[LRCLIB](https://lrclib.net/) for the words. That is Preferences → General →
*Look up lyrics on LRCLIB when the file has none*, **off by default**: on, the
artist, title, album and length of the track you are looking at are sent to a
server on the internet, and that is a thing to be asked about even for a
service that keeps no accounts and wants no key. The pane says where the words
came from when they are not the file's own. The file always wins; a tag is
never second-guessed.

Every answer is kept in the library's database, "not found" included, so a
track is asked about once — not once per session, and not once per keypress
as you arrow down a playlist. A hit is kept for good; a miss is asked again
after a week, because the service grows; a failure to reach the server is
remembered for a minute and no further. Requests go one at a time from a
worker thread, and a track selected while another is being asked about waits
its turn rather than opening a second connection.

The lookup is exact — LRCLIB's `/api/get`, matched on the names the track was
published under, with the length to within two seconds — rather than a search,
because a guessed match shown as the song's lyrics is worse than an honest
blank. `lrclibUrl` names the server, since it is open source and can be run at
home. Cog has no online lyrics; this is new work.

**Timed lyrics are followed.** When the words are LRC — `[01:23.45]` in front
of each line — the pane marks the line being sung and keeps it in view while
that track plays; for any other track it shows the same lines unmarked. LRC
can come from three places: the lyrics tag itself, which beets, MusicBee and
similar taggers fill with LRC rather than plain text; a `.lrc` file beside the
track, as `lrcget` and most lyrics plugins write it; and LRCLIB, whose timed
copy is preferred over its plain one. Between the file's own sources, timed
beats plain: the tag if it is LRC, then the `.lrc` file, then the tag as plain
text, and only then the service. Whether a tag is LRC is decided by reading
it — timed lines have to outnumber untimed ones — so plain lyrics with a
`[Chorus]` marker stay plain. The format's `[offset:]` header is honoured,
several stamps on one line repeat it, and enhanced LRC's per-word stamps are
dropped: the line is followed, not the syllable. A `.lrc` is looked for only
beside a local file that is one track — not for a cue sheet's span or a
subsong, which the file's times would not match. Following pauses while text
in the pane is selected, so copying a verse does not have it scrolled away.
View → *Timed Lyrics* (also Preferences → General, `lyricsSynced`) turns this
off: timed words are then shown as plain text with their stamps stripped, and
the tag's own words go back ahead of a `.lrc` file, since timing was the only
reason the file ranked above them.
ID3's binary SYLT frame is not read. Cog shows no timed lyrics either.

## Editing tags

*Edit Tags...* — in the playlist's right-click menu and in the Edit menu — writes
back to the file itself. FLAC, Ogg Vorbis, Opus and Speex, MP3, MP4 and ALAC,
Wav, AIFF, TrueAudio, WavPack, Monkey's Audio, Musepack, Matroska and the ASF
family it calls WMA: the list is long, and it is TagLib's, not ours. XPCog asks
the file rather than a table of formats — the tag editor's probe opens it and
asks TagLib whether this file can be saved — so a format TagLib supports is
editable here the day it is supported at all, and a file the probe declines has
no menu item to click. A cue sheet's track, an archive member, and a file that
is not local are all declined for the same reason: there is no file to write.

What the dialog shows is that file's tags, and its tags only. The fields every
player has a column for — artist, album, title, and the rest — come first, then
every other name the file carries, each on its own row with its value, then a
place to add names it does not carry, then the cover. Nothing is invented: open
a file with no tags and every row is blank.

**Any name, and more than one value where the format keeps more.** A row whose
name is not one of the standard ones is a row you can type any name into, so
MusicBrainz's IDs, `MUSICBRAINZ_ALBUMARTISTID`, `REPLAYGAIN_ALBUM_GAIN` or a
label's own catalogue code all work. Two or three names on one tag work too —
several artists on one track is the common case — written as `A, B` in the box
and stored as two values, which is what FLAC, Ogg, WavPack and Monkey's Audio
hold, and what an MP3 rewrite keeps as well.

Where a file cannot do something the box asks for, a dim line under that row
says which: *This format keeps only the first value.* for a field with one
slot, or *This format has no field for this name; it will be stored
generically.* for a name it has no frame for, which ID3v2 parks in a text
frame and MP4 in a vendor-namespaced atom, both spelled however TagLib likes.
Those words are the probe's answer about this file rather than a fixed list of
formats, and for a selection they describe what every file in it keeps. Writing
two artists to a FLAC file, on the other hand, is what FLAC is for.

**A selection, not only a file.** Select ten rows and the dialog edits all ten
at once. A row comes filled in only when every selected file agrees on it;
where they differ it starts blank and says so, because a box full of other
people's albums is an accident waiting to be saved. What you type then replaces
that field in every selected file, and the fields you never touched stay as
each file had them — which is what makes a batch pass able to fix ten files'
genre without flattening ten different albums. A save that reaches nothing —
a stream, a cue sheet's track, a format the probe declined — says how many were
skipped rather than pretending it wrote them.

**What the file carries, the save keeps.** Saving rewrites the file's tag block,
so a format that stores a field somewhere the reader does not look could quietly
drop it — which is why the write is handed everything back. For one file that is
literal: each name the file carries is a row, a row you never touched rides into
the write as it came, and the things no row can show — a second picture block,
bytes some reader found and named — pass through untouched instead of vanishing
under the strip. The cover is kept unless the cover section was used. For a
selection the writer merges instead, so a field you never touched is not written
to at all.

Spelling is the one thing that does not ride through unchanged. A field you edit
is written under the name the editor uses, and the spellings that lost go with
it, so an old `album artist` or `recording_date` is not left sitting beside the
new value; a name nobody touched keeps the spelling the file had it in. Opening
one file rewrites the whole block, so its standard fields come back spelled the
editor's way however you edited them.

The other thing worth knowing is that ID3v2 frames the reader never names are
not collateral damage. A popularimeter or a private frame is outside the tag map,
yet it survives both a single file's rewrite and a batch merge, because TagLib
keeps the frames it parsed even when it has no property to put them in; a frame
TagLib cannot parse at all is gone when it opens the file, not when it saves it.
Which version of the block gets written is a setting — `id3v2Version`, 3 or 4,
defaulting to 4, in the settings database rather than on the preferences pane —
for anyone whose car or amplifier reads version 3 only.

Lyrics are the one field with a catch outside the dialog. The lyrics row writes
`unsyncedlyrics`, and lyrics display does not stop at the tag: the plain view
shows the tag's own words, while the timed view falls back to a sidecar `.lrc`,
and then to LRCLIB, when the tag carries no timestamps. Type timestamps into that
row and the tag wins — a tag that parses as `.lrc` is read as timed lyrics from
the file itself.

Cover art comes from a file on disk: *Choose* replaces the picture, *Remove*
takes it out. The dialog does not crop, resize or transcode; it stores what was
chosen.

A save is a write to disk, so the playlist re-reads what it wrote afterwards:
what the columns show is what the file now holds, not what the dialog was
told. If a file that appears twice in a playlist was written once, both rows
change, because both are the same file.

Cog never wrote tags at all, so none of this is port work. `docs/TAGEDITOR.md`
has the design and the reasons behind the write semantics.

## Remote control

A REST API over the transport, the playlist, the equaliser, the settings and the
cover art, with a generated OpenAPI 3.1 document and a Swagger UI page beside it.
Preferences → Remote turns it on; `docs/REST.md` is the reference.

```sh
curl -H "Authorization: Bearer $TOKEN" localhost:7799/api/v1/status
xdg-open http://localhost:7799/docs
```

**Off in a default build, and off again at run time.** `app/src/SingleInstance.hpp`
records a decision against this program owning a listening socket at all — on Windows
the firewall asks the user to approve a *music player* wanting network access, which is
alarming and unanswerable — so the feature is opt-in twice: `XPCOG_WITH_REST` decides
whether a server is compiled (the presets turn it on), and `remoteEnable` decides
whether it ever binds. The default bind is loopback, which raises no prompt.

Every request needs a bearer token, with no exemption for local connections; the token
is kept in the system password store rather than in settings. There is no TLS and none
planned — a self-signed certificate on a LAN is theatre — so the connection is not
encrypted and the pane says so before you bind it to the network.

`xpcog-cli serve` runs the same API with no toolkit linked at all, which is what
demonstrates that none of this reached below the interface layer.

## Languages

The interface speaks **English** and **Spanish**, and follows the system by
default. Preferences → General has the picker; it applies the next time XPCog
starts, because a catalogue is chosen once, before the first window is built.

The translations live in [`app/locale`](app/locale) as ordinary gettext `.po`
files and are compiled into the binary — there is nothing to install beside the
executable and nothing that can go missing on one machine and not another. Adding
a language is one `.po`, one line of CMake and one row of a table;
[`app/locale/README.md`](app/locale/README.md) is the whole procedure, including
what is deliberately *not* translated and why.

Two conventional tools are replaced by two small ones, for the same reason:
gettext is not a dependency this project has anywhere else, and Windows is the
platform least likely to have it. `tools/extract-messages.py` stands in for
`xgettext`, and `cmake/CompileCatalog.cmake` for `msgfmt`. Neither is on the
build's critical path except the second, which is a CMake script.

`core`, `codecs` and `platform` are not translated and cannot be: they link no
toolkit, and so have no catalogue to consult. The handful of strings they produce
that a listener reads — the playlist's column headings — are mapped in the app
layer, which is what `PluginRegistry` and `PlaylistView` were already written to
allow.
