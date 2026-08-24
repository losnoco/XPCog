# miniaudio rate probe

Answers one question: **when XPCog asks miniaudio for a sample rate, does the
hardware actually run at it?**

`ma_device` carries two rates. `device.sampleRate` is the one the caller gets to
think it is; `device.playback.internalSampleRate` is the one the hardware is
running. When they differ, miniaudio has silently inserted a data converter
between them — and its default resampler is **linear**
(`ma_device_config_init()` sets `ma_resample_algorithm_linear`, and XPCog has
never overridden it).

`MiniaudioOutput` reports the first of those through `negotiatedFormat()`, which
is why the difference is invisible from inside the engine: `AudioConverter` is
told the rate matches, correctly declines to resample, and miniaudio resamples
anyway.

## Why it is not a build target

It talks to miniaudio directly rather than through `IAudioOutput`, because the
whole question is about a field that seam does not expose — a probe that went
through it could only repeat what the seam already says. Nothing in the tree
depends on this, and nothing should.

## Running it

Needs a build, for the vendored miniaudio object. macOS:

```sh
clang++ -std=c++20 -O2 -I vendor/miniaudio \
    -o /tmp/ma-rate-probe tools/ma-rate-probe/probe.cpp \
    build/macos-debug/vendor/miniaudio/libvendor-miniaudio.a \
    -framework CoreFoundation -framework CoreAudio -framework AudioToolbox \
    -framework AudioUnit -framework Foundation

/tmp/ma-rate-probe
```

Linux links `-lpthread -lm -ldl`; Windows needs no extra libraries.

It opens the **default** device and makes no sound.

## What was found, 2026-08-23, macOS

The device was at 44,100 Hz. Shared mode kept it there and converted everything
else into it; exclusive mode switched the hardware to whatever was asked for.

```
shared:
   44100 Hz : device=44100  internal=44100   no conversion
   48000 Hz : device=48000  internal=44100   *** miniaudio is resampling (linear) ***
   96000 Hz : device=96000  internal=44100   *** miniaudio is resampling (linear) ***
  192000 Hz : device=192000 internal=44100   *** miniaudio is resampling (linear) ***

exclusive:
   48000 Hz : device=48000  internal=48000   no conversion
  192000 Hz : device=192000 internal=192000  no conversion
```

Read it the right way round: **exclusive mode is the one that behaves.** The
shared path — which is the default, since `exclusiveOutput` is false — runs
every track whose rate is not the device's current one through a linear
resampler, while soxr sits unused one layer up.

Re-run it after changing the device's rate in Audio MIDI Setup (or the Windows
mixer) and the whole table shifts with it, which is what says the number being
matched against is the device's *current* nominal rate rather than anything
about the file.
