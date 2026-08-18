"""A fake SHOUTcast station, for exercising XPCog's HTTP source by hand.

Endless, like the real thing: it cycles a handful of short "tracks", each an
audibly different waveform, and announces each one's StreamTitle in the
interleaved metadata at the moment the track changes. That is what makes it a
test of the live-title path rather than of header parsing -- the title has to
arrive mid-stream, between two metaint blocks, and the window has to notice.

It answers "ICY 200 OK" rather than an HTTP status line, which is not HTTP and
is the case CURLOPT_HTTP09_ALLOWED exists for in codecs/httpsource.

  python tools/fake-radio/fake-radio.py [port]

then open http://127.0.0.1:<port>/stream.wav in XPCog. Check the stream itself
with icycheck.py before concluding anything about the player -- see README.md.
"""

import itertools
import math
import socket
import struct
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8732

RATE = 44100
CHANNELS = 2
BYTES_PER_FRAME = CHANNELS * 2
BYTE_RATE = RATE * BYTES_PER_FRAME
METAINT = 8192

TRACK_SECONDS = 12

# name, left frequency, right frequency, waveform
TRACKS = [
    ("The Tone Collective - Sine in A", 440.0, 440.0, "sine"),
    ("Square Wave Quartet - Study in E", 330.0, 495.0, "square"),
    ("Triangle Trio - Slow Descent", 262.0, 392.0, "triangle"),
    ("Saw Ensemble - Rising Fifth", 220.0, 330.0, "saw"),
]


def wave_sample(kind, phase):
    """One cycle of `kind` at `phase` in [0, 1), peak 1.0."""
    if kind == "sine":
        return math.sin(2.0 * math.pi * phase)
    if kind == "square":
        return 1.0 if phase < 0.5 else -1.0
    if kind == "triangle":
        return 4.0 * abs(phase - 0.5) - 1.0
    return 2.0 * phase - 1.0  # saw


def render(left_hz, right_hz, kind):
    """TRACK_SECONDS of 16-bit stereo PCM, with short fades at both ends so a
    track change is a musical event rather than a click."""
    frames = RATE * TRACK_SECONDS
    fade = int(RATE * 0.05)
    out = bytearray()
    for i in range(frames):
        gain = 0.30
        if i < fade:
            gain *= i / fade
        elif i > frames - fade:
            gain *= (frames - i) / fade
        left = wave_sample(kind, (i * left_hz / RATE) % 1.0) * gain
        right = wave_sample(kind, (i * right_hz / RATE) % 1.0) * gain
        out += struct.pack("<hh", int(left * 32767), int(right * 32767))
    return bytes(out)


def wav_header():
    """A streaming WAV header: the sizes are deliberately enormous because the
    stream has no end to measure."""
    endless = 0x7FFFFF00
    return (b"RIFF" + struct.pack("<I", endless) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, CHANNELS, RATE,
                        BYTE_RATE, BYTES_PER_FRAME, 16) +
            b"data" + struct.pack("<I", endless))


def meta_block(text):
    """The length byte plus `text` padded to whole sixteen-byte units."""
    raw = text.encode("utf-8")
    units = (len(raw) + 15) // 16
    return bytes([units]) + raw.ljust(units * 16, b" ")


print("rendering %d tracks..." % len(TRACKS), flush=True)
RENDERED = [(name, render(left, right, kind)) for name, left, right, kind in TRACKS]


class Streamer:
    """Everything the client sees after the ICY headers goes through here.

    The metaint cycle counts *stream* bytes, and the WAV header is part of that
    stream. Sending it around this accounting put every metadata block 44 bytes
    late for the life of the connection, which a correct client dutifully reads
    as a length byte in the middle of the audio: continuous glitching, and
    titles that only sometimes survived. One send path makes that
    unrepresentable.
    """

    # How much goes out before pacing begins, so the player has something banked
    # the way a real station's burst-on-connect provides.
    PREROLL = BYTE_RATE * 3

    def __init__(self, conn):
        self.conn = conn
        self.since_meta = 0
        self.sent = 0
        self.title = None
        self.announced = None
        self.started = time.monotonic()

    def set_title(self, title):
        self.title = title

    def send(self, data):
        offset = 0
        while offset < len(data):
            take = min(len(data) - offset, METAINT - self.since_meta)
            self.conn.sendall(data[offset:offset + take])
            offset += take
            self.sent += take
            self.since_meta += take

            if self.since_meta == METAINT:
                # A title only when it actually changed; otherwise the single
                # zero byte meaning "nothing new", which is what real stations
                # send the vast majority of the time.
                if self.title != self.announced:
                    self.conn.sendall(meta_block("StreamTitle='%s';" % self.title))
                    self.announced = self.title
                else:
                    self.conn.sendall(bytes([0]))
                self.since_meta = 0

            self.pace()

    def pace(self):
        # Against an absolute schedule rather than sleeping per chunk: a
        # per-chunk sleep overshoots a little every time on Windows, and a
        # station running even two percent slow drains the player's buffers and
        # produces dropouts that look exactly like a bug in the player.
        target = self.started + (self.sent - self.PREROLL) / float(BYTE_RATE)
        delay = target - time.monotonic()
        if delay > 0:
            time.sleep(delay)


def serve(conn, peer):
    print("connection from %s:%d" % peer, flush=True)
    try:
        conn.recv(4096)  # the request; the answer is the same regardless
        headers = (
            "ICY 200 OK\r\n"
            "icy-name: XPCog Test Radio\r\n"
            "icy-genre: Test Tones\r\n"
            "icy-url: http://example.invalid/\r\n"
            "icy-metaint: %d\r\n"
            "content-type: audio/x-wav\r\n"
            "\r\n" % METAINT)
        conn.sendall(headers.encode("ascii"))

        stream = Streamer(conn)
        stream.set_title(RENDERED[0][0])
        stream.send(wav_header())

        for name, pcm in itertools.cycle(RENDERED):
            print("  now playing: %s" % name, flush=True)
            stream.set_title(name)
            stream.send(pcm)
    except OSError as error:
        print("  %s:%d disconnected (%s)" % (peer[0], peer[1], error), flush=True)
    finally:
        conn.close()


server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", PORT))
server.listen(5)
print("listening on http://127.0.0.1:%d/stream.wav" % PORT, flush=True)
print("%d tracks, %ds each, title announced at every change"
      % (len(TRACKS), TRACK_SECONDS), flush=True)

while True:
    # A reset arriving during the handshake raises out of accept() itself on
    # Windows; an unguarded loop dies on the first one.
    try:
        conn, peer = server.accept()
    except OSError:
        continue
    threading.Thread(target=serve, args=(conn, peer), daemon=True).start()
