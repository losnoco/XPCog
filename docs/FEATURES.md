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
store — Credential Manager, the Keychain, the Secret Service — through
`wxSecretStore`, not in the settings.

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

The switch itself is `enableAudioScrobbler`, which is Cog's key — but it is the
one setting a Cog import deliberately does **not** carry across, because the
credential cannot come with it and the switch alone would claim a connection that
does not exist.

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
