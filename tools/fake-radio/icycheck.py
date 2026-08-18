"""Reads a station the way a correct ICY client must, and reports whether the
stream it is actually sending is well formed and on time.

This exists because the first version of fake-radio.py was subtly broken and the
symptoms -- glitching audio, titles that only sometimes appeared, dropouts --
all looked exactly like bugs in the player. Two questions have to be answered
about the *fixture* before any of them is asked about XPCog:

  1. Is the metaint framing consistent? A desynchronised stream shows up as
     metadata length bytes that decode to junk instead of a StreamTitle -- which
     is what a client would be feeding its decoder as audio.
  2. Is it delivered at real time? A station running even a few percent slow
     drains the player's buffers and produces dropouts indistinguishable from a
     fault in the player.

  python tools/fake-radio/icycheck.py [port] [seconds]
"""

import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8732
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

BYTE_RATE = 44100 * 2 * 2

sock = socket.create_connection(("127.0.0.1", PORT), timeout=10)
sock.sendall(b"GET /stream.wav HTTP/1.0\r\nIcy-MetaData: 1\r\nUser-Agent: icycheck\r\n\r\n")

buf = b""
while b"\r\n\r\n" not in buf:
    chunk = sock.recv(4096)
    if not chunk:
        raise SystemExit("server closed during headers")
    buf += chunk

head, body = buf.split(b"\r\n\r\n", 1)
print("--- headers ---")
print(head.decode("latin-1").strip())

metaint = 0
for line in head.split(b"\r\n"):
    if line.lower().startswith(b"icy-metaint:"):
        metaint = int(line.split(b":", 1)[1])
print("--- metaint = %d ---" % metaint)

start = time.monotonic()
audio_bytes = 0
since_meta = 0
titles = []
bad_blocks = 0
blocks = 0

# State machine mirroring IcyDemux: audio, length byte, metadata.
pending = body
phase = "audio"
meta_left = 0
meta_buf = b""

while time.monotonic() - start < SECONDS:
    if not pending:
        try:
            pending = sock.recv(16384)
        except socket.timeout:
            print("!! recv timed out -- server stalled")
            break
        if not pending:
            print("!! server closed the connection")
            break

    if phase == "audio":
        take = min(len(pending), metaint - since_meta)
        audio_bytes += take
        since_meta += take
        pending = pending[take:]
        if since_meta == metaint:
            phase = "length"
            since_meta = 0
    elif phase == "length":
        meta_left = pending[0] * 16
        pending = pending[1:]
        meta_buf = b""
        blocks += 1
        phase = "meta" if meta_left else "audio"
    else:
        take = min(len(pending), meta_left)
        meta_buf += pending[:take]
        pending = pending[take:]
        meta_left -= take
        if meta_left == 0:
            text = meta_buf.rstrip(b" ").decode("ascii", "replace")
            if "StreamTitle='" in text:
                at = time.monotonic() - start
                titles.append((at, text))
                print("  [%6.2fs] %s" % (at, text))
            elif text:
                bad_blocks += 1
                print("  !! block %d is not a StreamTitle: %r" % (blocks, text[:60]))
            phase = "audio"

elapsed = time.monotonic() - start
rate = audio_bytes / elapsed if elapsed else 0

print("--- result ---")
print("audio delivered : %d bytes in %.1fs" % (audio_bytes, elapsed))
print("effective rate  : %.0f B/s (real time is %d B/s)" % (rate, BYTE_RATE))
print("speed           : %.1f%% of real time" % (100.0 * rate / BYTE_RATE))
print("metadata blocks : %d (%d titles, %d malformed)"
      % (blocks, len(titles), bad_blocks))

if rate < BYTE_RATE * 0.98:
    print("VERDICT: too slow -- a player will starve and drop out")
elif bad_blocks:
    print("VERDICT: framing is desynchronised -- audio is being read as metadata")
else:
    print("VERDICT: stream is well formed and on time")
