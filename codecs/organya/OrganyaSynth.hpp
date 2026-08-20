// The Organya player: Cave Story's music format, synthesised.
//
// Port of the player embedded in Cog Plugins/Organya/OrganyaDecoder.mm, which
// is itself Joel Yliluoma's "simple Cave Story music player" (iki.fi/bisqwit)
// written against NX-Engine as a reference. Nothing here is a library: the
// format is small enough that the whole player is about four hundred lines, and
// Cog carries the same four hundred lines inline.
//
// There is no upstream to track and no Cog framework to vendor, so this lives
// in codecs/ rather than vendor/ -- the rule is in ports/README.md, and a
// player that exists only as part of one plugin's source file is the case that
// rule sends here.
//
// An .org file is a sequencer, not a recording. Sixteen instrument tracks, the
// first eight melodic and the last eight percussive, each a sparse map from
// beat number to a note event. Melodic notes play a 256-sample waveform picked
// out of a bank of a hundred (`wavetable.dat`); percussion plays a sample
// synthesised at startup by a miniature FM/AM engine from PixTone, Pixel's
// sound-effect tool, driven by six parameter files (`fx9*.pxt`). Both sets of
// data ship with this codec -- see CMakeLists.txt for why they are compiled in
// rather than installed.
//
// The format has no end. It has a loop point and a loop end, and it plays
// between them for ever; a length has to be invented, which is the decoder's
// job rather than this file's.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace xpcog::organya {

/// The bank of a hundred 256-sample melodic waveforms and the twelve
/// synthesised percussion samples, built once per process.
///
/// Both are derived from data that never changes, and building the percussion
/// means running the PixTone synthesiser over six parameter sets -- cheap, but
/// not something to repeat per file when a playlist holds two hundred of them.
struct Tables {
    /// 100 waveforms of 256 samples, held as shorts because that is what the
    /// mixer indexes; every value is in signed-char range.
    std::vector<std::int16_t> waveTable;

    /// Twelve drums, of which six are empty: the patch table names files for
    /// slots 0, 2, 4, 5, 6 and 8 only, and a note on any other slot is silent.
    std::vector<std::int16_t> drums[12];

    /// Built on first call and shared thereafter. Never null.
    [[nodiscard]] static const Tables& shared();
};

/// One note event, exactly as it sits in the file. 255 means "leave this
/// parameter where it was", which is why these are plain ints and not clamped.
struct Event {
    int note    = 255;
    int length  = 0;
    int volume  = 255;
    int panning = 255;
};

/// One of the sixteen tracks: its instrument settings, its events, and the
/// state of whatever note it is in the middle of playing.
struct Instrument {
    // From the file.
    int         tuning = 0;
    int         wave   = 0;
    /// "Pi" mode: every note plays for exactly 1024 wavetable samples,
    /// regardless of the length written on the event.
    bool        pi     = false;
    std::map<int, Event> events;

    // Changed during playback.
    double              phaseacc    = 0.0;
    double              phaseinc    = 0.0;
    double              curVolume   = 0.0;
    int                 curPan      = 0;
    std::int64_t        curLength   = 0;
    std::int64_t        curWaveSize = 0;
    const std::int16_t* curWave     = nullptr;
};

/// A loaded .org file, positioned at a beat.
class Song {
public:
    /// Parses the header and every event list. Returns false for anything that
    /// is not an "Org-02" file or is truncated partway through.
    ///
    /// `sampleRate` fixes samplesPerBeat() for the life of the object: the
    /// format counts time in milliseconds per beat and the mixer counts it in
    /// whole samples, and the conversion truncates. Re-deriving it per beat
    /// (which is what Cog does) gives the same answer every time, so there is
    /// nothing to gain by not settling it here.
    [[nodiscard]] bool load(std::span<const std::byte> data, double sampleRate);

    /// Puts every track back to silence at beat zero.
    void reset();

    /// Advances one beat, writing `samplesPerBeat()` interleaved stereo frames
    /// to `out` -- or, when `out` is null, advancing every track's playback
    /// state by exactly what rendering would have advanced it by, without
    /// mixing a single sample.
    ///
    /// The null form is what makes seeking cheap *and* correct. A track's state
    /// after a beat is (phase advanced by the sample count, remaining note
    /// length reduced by the same), and neither depends on the samples that
    /// came out -- the mixer reads the waveform and never writes back to it. So
    /// skipping the mix is not an approximation of playing the beat, it is the
    /// same arithmetic with the audio thrown away before it is computed rather
    /// than after. Cog instead jumps `cur_beat` straight to the target, which
    /// loses every note still sounding across the seek point.
    ///
    /// `out` is written, not accumulated: the caller need not clear it.
    void renderBeat(float* out);

    [[nodiscard]] std::size_t samplesPerBeat() const { return samplesPerBeat_; }
    [[nodiscard]] int         msPerBeat() const { return msPerBeat_; }
    [[nodiscard]] int         loopStart() const { return loopStart_; }
    [[nodiscard]] int         loopEnd() const { return loopEnd_; }
    [[nodiscard]] int         beat() const { return beat_; }

private:
    /// Applies whatever event this beat holds for `track`, if any.
    void startNote(Instrument& track, std::size_t index);

    static constexpr std::size_t kTracks = 16;
    /// Tracks 8..15 are percussion; they index the drum bank rather than the
    /// waveform bank and take their pitch linearly rather than by temperament.
    static constexpr std::size_t kFirstDrumTrack = 8;

    const Tables* tables_ = nullptr;

    int    msPerBeat_      = 0;
    int    loopStart_      = 0;
    int    loopEnd_        = 0;
    double sampleRate_     = 0.0;
    std::size_t samplesPerBeat_ = 0;

    int        beat_ = 0;
    Instrument tracks_[kTracks];
};

}  // namespace xpcog::organya
