# The REST remote control

An HTTP/JSON API over the transport, the playlist, the DSP chain, the settings
and the cover art, with a generated OpenAPI 3.1 document and a Swagger UI page
served beside it.

It is off in a default build and off again at run time. `XPCOG_WITH_REST`
decides whether a server is compiled; `remoteEnable` decides whether it ever
binds; and the bind defaults to loopback. See **Why the socket is answered
twice** below — that arrangement is deliberate and it is the point of the
feature's shape.

```sh
# In the player: Preferences -> Remote. Or headless:
xpcog-cli serve --port 8080

curl -H "Authorization: Bearer $TOKEN" localhost:8080/api/v1/status
xdg-open http://localhost:8080/docs
```

## The shape of it

```
                       ┌─ app/src/AppPlayerControl   (wx; hops via CallAfter)
core/src/remote/ ──────┤
  Router + route table └─ tools/cli/CliPlayerControl (SerialExecutor stands in
  OpenApi                                             for the interface thread)
  CallGate ── calls ──▶ IPlayerControl   (pure interface, core, no JSON)
  RemoteServer (cpp-httplib, behind a pimpl)
```

`RemoteServer::handle()` is the whole request pipeline — auth, routing,
validation, dispatch, serialisation — and it takes a struct and answers with a
struct. cpp-httplib sits outside it and does nothing but convert. That is why
almost every test of this subsystem binds no socket at all, and why the one that
does is testing the socket rather than the API.

## Threading, and why there is a gate

Requests arrive on cpp-httplib's threads. Nearly nothing in this program may be
touched from one: `Playlist`, `PlaylistView`, `UndoStack`, `Library`, `Settings`
and `Signal` are unlocked and single-threaded by convention, and `PluginCache` is
explicitly unsynchronised.

`platform::MediaIntegration` had already solved half of this — take a
`Dispatcher`, hop to the interface thread — but it is fire-and-forget, and an
HTTP request has to come back with something. `CallGate` is the difference. It
posts the work and waits, with three details that are not decoration:

- **The shared state is heap-allocated and captured by value**, never a
  `std::promise` on the caller's stack. `wxEvtHandler::CallAfter` drops pending
  events when the handler dies, and a merely slow interface thread runs the
  closure *after* the wait has timed out. Either would be a use-after-free with a
  delay on it.
- **Dispatch happens before the lock is taken**, so a dispatcher that runs its
  callable inline — `xpcog-cli`'s, and every test's — does not deadlock against
  itself.
- **Two seconds, then 503** with `Retry-After`. Not 504: nothing here is a
  gateway, the interface is busy. A modal dialog does *not* cause this — wx
  modals pump a nested event loop, so `CallAfter` still runs — and the real
  causes are a long synchronous handler and shutdown.

`GET /status` takes no hop at all: the interface pushes a snapshot in, the way
`MainFrame` already pushes `setNowPlaying` into `MediaIntegration`. Polling it
costs an atomic load.

**Nothing on the interface thread may call `handle()`.** It would wait on itself.

## Saying what actually happened

Two places where the easy answer would be a lie, and neither is given.

**409, not 200, for a command that was declined.** `PlaybackController` guards a
start in flight and silently ignores commands while one is running — right for a
menu item, where the gesture is cheap to repeat, and wrong for something that has
to report an outcome. `PlaybackController::busy()` exists so this can be reported.

**`appliesFrom` on every settings and DSP response.** An equaliser band moves
what is already playing; a ReplayGain mode is read when the next track opens and
cannot. The five answers are `immediately`, `nextTrack`, `nextDeviceOpen`,
`nextScan` and `nextLaunch`, and they come from `app/src/SettingEffect.cpp` —
the same table the window's own fan-out switches on, so the API and the player
cannot disagree about it.

## Track ids are scoped to a session

Nothing persists a `TrackId`. The same number means a different track after a
restart, so a client that cached one and acted on it later would edit the wrong
thing. `Status` carries a `sessionId`, random per launch, and a
`playlistRevision` bumped on every change. **When either moves, re-read.**

## Authentication

A bearer token on every request, checked in constant time, with **no exemption
for loopback** — that would hand every process on the machine the transport.

Missing, malformed and wrong produce the same 401 down to the byte: a client that
could tell them apart could learn about the token by asking. After five failures
in a minute a peer waits a quarter of a second for each further one, which is the
difference between a 64-character token and one that can be tried at line rate.

The token is 32 bytes from the operating system's cryptographic generator, as 64
hex characters. In the player it lives in `wxSecretStore`, for the reason
`LastFmAccount.hpp` gives about the Last.fm session key: settings are a registry
key or a plist — readable text, backed up, synced — and not where a credential
goes. **With no secret store the server does not start**, and the pane says so;
falling back to `settings.def` is refused deliberately.

`xpcog-cli serve` takes `--token-file`, then `$XPCOG_REMOTE_TOKEN`, then
generates one and prints it.

### What is *not* behind the token

`/docs` and the three files it loads, and only those. A browser cannot put an
`Authorization` header on a top-level navigation, so a token-gated documentation
page is one nobody can open. What that exposes is four static files describing
the page's own chrome; `/openapi.json` and every endpoint still need the token,
and the page asks for one, keeps it in `sessionStorage` for the tab, and attaches
it to the specification fetch and every try-it-out call itself.

The page's own script is a file rather than an inline block, because the page is
served with `script-src 'self'` and that blocks inline execution outright.

The residual risk, named rather than left implicit: a hostile page could reach
those four files through DNS rebinding. It can reach nothing else — there are no
CORS headers anywhere and no ambient credential to borrow.

## The security posture, stated plainly

- **Off twice over.** Not built without `XPCOG_WITH_REST`; not listening without
  `remoteEnable`.
- **Loopback by default.** Binding wider is a choice the pane offers and labels.
- **No TLS, and none planned.** A self-signed certificate on a LAN is theatre —
  nothing verifies it, so it stops no one — and it would be a dependency and a
  certificate lifetime to manage for that. **The connection is not encrypted and
  the token travels in a header on every request.** Use it on a network you
  trust, or over a tunnel you already have.
- **No CORS headers at all.** A page on another origin cannot reach the API even
  holding a stolen token.
- **No mDNS or discovery.** The player does not announce itself.
- **`X-Content-Type-Options: nosniff` and `Cache-Control: no-store`** on
  everything.

### What a token grants

Control of playback and the playlist, and — through `POST /playlist/tracks`
followed by `GET /playlist/{id}` — **read access to the tags of anything the
player can decode, anywhere it can reach.** That is inherent in being able to add
a track, and it is what the token is protecting.

What it deliberately does *not* grant, and the interface header says so: revealing
a file in a file manager, moving one to the trash, or writing a playlist to a
path the request names. Adding a track implies arbitrary *read*; filesystem write
and delete driven by a network peer are a different order of consequence and stay
in the window.

## Why the socket is answered twice

`app/src/SingleInstance.hpp` records a decision against this program owning a
listening socket at all:

> A listening TCP socket on Windows means the firewall asks the user to approve a
> *music player* wanting network access, which is alarming, unanswerable and
> entirely self-inflicted.

That argument has not gone away, and this feature is shaped by it rather than
around it. The answer is the one the crash reporter already uses: opt in twice.
The build option decides whether a server exists; the setting decides whether it
listens; and the default bind is loopback, which raises no firewall prompt.

## Endpoints

The generated document at `/openapi.json` is the reference — it is produced by
walking the same table the router dispatches from, so it cannot drift. In outline:

| Group | Endpoints |
| --- | --- |
| Transport | `GET status`; `POST transport/{play,pause,playPause,stop,next,previous}`; `POST transport/seek`; `GET\|PUT transport/volume`; `GET\|PUT transport/order` |
| Playlist | `GET playlist` (`offset`, `limit`, `q`); `GET\|PATCH playlist/{id}`; `GET playlist/{id}/artwork`; `POST\|DELETE playlist/tracks`; `POST playlist/{move,randomize,undo,redo}`; `DELETE playlist`; `POST\|DELETE playlist/queue` |
| Jobs | `GET jobs/{id}` |
| DSP | `GET\|PUT dsp/equalizer`; `GET dsp/equalizer/presets`; `POST dsp/equalizer/preset` |
| Settings | `GET settings`; `PATCH settings`; `GET\|PUT settings/{key}` |
| Meta | `GET version`; `GET /openapi.json`; `GET /docs` |

Errors are uniformly `{"error":{"code":"…","message":"…","field":"…"}}` and are
**untranslated**: core has no catalogue and never will, and JSON keys and error
codes are protocol rather than interface text.

A few behaviours worth knowing without reading the whole document:

- `POST /playlist/tracks` answers **202** and a job id. A directory of ten
  thousand files is not something to hold a request open for.
- `?q=` filters the response and **does not touch the player's own filter box**.
  A remote read that changed what the user is looking at would be a bug.
- `PATCH /settings` answers **207** when some key was refused and the rest took.
  Neither 200 nor 400 is true of that.
- Session state — `settingsSchemaVersion`, `lastPlaybackStatus`, `miniMode` and
  the rest — is readable and **not writable**, the same rule the Advanced pane
  applies. What the last session did is not a preference.
- Playlist edits go through the same undo stack the Edit menu drives, so a track
  deleted from a phone is undone with Ctrl+Z in the window. The label says
  `(remote)` so it is clear where it came from. The per-entry flags — queue,
  stop-after, rating, play count — are not undoable, because they are not
  undoable from the window either.

## What `xpcog-cli serve` cannot do

It exists to prove the seam: a target that links no toolkit at all driving the
same routes. What it cannot do answers **501**, and is listed here and in
`--help` rather than discovered.

| Missing | Why |
| --- | --- |
| Skipping a track that will not open | The hunt for a playable entry is `PlaybackController`'s, and it is app-layer. A bad file stops playback here. |
| `409 Busy` | No `starting_`/`stopping_` guards. The executor serialises instead, so a slow URL holds its queue and requests time out at the gate as 503. |
| Cover art, ratings | Both live in the SQLite library, which this host does not open. |
| Output-device switching under a running stream, resume-at | `PlaybackController`'s. |
| Translated undo labels | core has no catalogue. |
| Any desktop integration | There is no desktop. |

Moving `PlaybackController` into core would collapse most of that table; its only
toolkit dependencies are a `wxTimer` and the bare `wxEvtHandler` that owns it,
both replaceable by an injected tick source. That is a real and worthwhile
refactor, and it is not this feature's.
