# Fake radio

A SHOUTcast station on localhost, for exercising the HTTP source
(`codecs/httpsource`) by hand: internet radio is the case the unit tests cannot
reach, because what makes it hard is a live socket, a clock, and a title that
changes while audio is playing.

```sh
python tools/fake-radio/fake-radio.py          # serves on 8732
```

Then **File → Open URL** (Ctrl-L) in XPCog and enter:

```
http://127.0.0.1:8732/stream.wav
```

Four twelve-second tracks cycle forever, each an audibly different waveform, and
each announces its own `StreamTitle` at the changeover. Between announcements the
station sends the single zero byte meaning "nothing new", which is what real
stations send the vast majority of the time. Two of the four are a square and a
saw and are *meant* to sound harsh; the sine and the triangle are the ones that
should be clean.

What it covers that a file cannot:

- the `ICY 200 OK` status line, which is not HTTP and which libcurl only accepts
  under `CURLOPT_HTTP09_ALLOWED`, so the response arrives as an HTTP/0.9 body and
  the ICY headers have to be parsed out of the first bytes of audio;
- `icy-metaint` de-interleaving, live, across arbitrary packet boundaries;
- a title that changes *during* playback, which is the only metadata path with no
  single moment at which it can be read;
- a stream with no length: not seekable, no duration, and it never ends;
- stopping and switching away from a stream that is mid-flight.

## Check the station before blaming the player

```sh
python tools/fake-radio/icycheck.py 8732 40
```

`icycheck.py` reads the stream exactly as a correct client must and reports two
things: whether the metaint framing is consistent, and whether the audio is
delivered at real time. **Run it before concluding anything about XPCog.**

That is not a precaution invented in advance -- it is the tool that settled a real
session. The first version of this station sent the WAV header *around* its
metaint accounting, so every metadata block landed 44 bytes late for the life of
the connection. A correct client dutifully reads a byte of audio as a metadata
length and hands the rest to its decoder, and the symptoms were glitching audio,
titles that only sometimes appeared, and dropouts -- three separate-looking
faults, all of them in the fixture, all of them indistinguishable from bugs in
the player. The station now routes every stream byte through one accounting path
so that class of bug cannot be expressed.

A healthy run ends:

```
metadata blocks : 926 (4 titles, 0 malformed)
VERDICT: stream is well formed and on time
```

Rate slightly above 100% of real time is expected and correct: the station sends
three seconds unpaced on connect, the way a real one's burst fills a client's
buffer, then paces against an absolute schedule. Pacing by sleeping per chunk
instead drifts slow on Windows, and a station a few percent slow starves the
player into dropouts that look like its fault.

## Why it is not a build target, or a ctest

It needs a listening socket, wall-clock time and, to be worth running, a pair of
ears. The parts that can be checked without those already are: `StreamBuffer` and
`IcyDemux` are unit-tested in `tests/codecs/test_httpsource.cpp`, including
metaint framing across every awkward packet boundary, and the engine's side is in
`tests/core/test_stream_metadata.cpp`. This is for the rest — and for the times
the answer turns out to be that the test rig was wrong.

## Verifying the audio path itself

`xpcog-cli` decodes a stream to raw floats, which is how the byte path was shown
to be exact rather than merely plausible:

```sh
timeout 11 xpcog-cli decode http://127.0.0.1:8732/stream.wav live.pcm
```

Track one is a 440 Hz sine at 0.30 peak, so its sample-to-sample step cannot
exceed `2*pi*440/44100*0.30` = 0.0188. Measuring the maximum step over the sine
window and finding exactly that, with no discontinuities, says the whole path —
libcurl, ICY de-interleaving, the ring, the decoder, the converter — did not drop
or insert a single byte. Do not measure across a track boundary: the square wave's
jumps are real.
