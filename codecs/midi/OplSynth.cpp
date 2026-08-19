#include "midi/OplSynth.hpp"

#include <interface.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace xpcog::codecs {
namespace {

/// Extended panning, which both drivers gate on this flag.
///
/// A real OPL3 pans a channel left, centre or right and nothing between; the
/// registers at 0x106/0x108 are Nuked's extension, giving the continuous pan a
/// MIDI CC 10 actually asks for. So it is an improvement on the hardware rather
/// than an emulation of it -- and it is on because Cog has it on (MSPlayer is
/// constructed with set_extp(1) for both drivers, MIDIDecoder.mm:311), so a
/// file that sets pan sounds here the way it does there.
constexpr unsigned kExtendedPanning = 1;

/// The chip's own rate. Every driver renders at this and resamples; a caller
/// asking for something absurd would still work, but nothing is gained below
/// it and the resampler is what costs the time above it.
constexpr double kMinSampleRate = 1000.0;
constexpr double kMaxSampleRate = 384000.0;

[[nodiscard]] midisynth* create(OplDriver driver) {
    return driver == OplDriver::Doom ? getsynth_doom() : getsynth_opl3w();
}

}  // namespace

OplSynth::~OplSynth() { close(); }

void OplSynth::close() {
    delete synth_;
    synth_     = nullptr;
    sampleRate_ = 0.0;
}

const char* OplSynth::displayName() const noexcept {
    return driver_ == OplDriver::Doom ? "Nuked OPL3 (DMX)" : "Nuked OPL3 (General MIDI)";
}

bool OplSynth::open(OplDriver driver, unsigned bank, double sampleRate) {
    close();
    if (!(sampleRate >= kMinSampleRate && sampleRate <= kMaxSampleRate)) {
        return false;
    }

    std::unique_ptr<midisynth> synth{create(driver)};
    if (!synth) {
        return false;
    }

    // Out of range would leave the driver's instrument pointers unset, and a
    // note-on would then read through them. Clamped rather than rejected: an
    // unknown bank in a settings file should give the default instrument set,
    // not refuse to play the file.
    const unsigned banks = std::max(1u, bankCount(driver));
    bank                 = std::min(bank, banks - 1);

    if (!synth->midi_init(static_cast<unsigned>(std::lround(sampleRate)), bank,
                          kExtendedPanning)) {
        return false;
    }
    synth_      = synth.release();
    driver_     = driver;
    bank_       = bank;
    sampleRate_ = sampleRate;
    return true;
}

void OplSynth::reset() {
    if (synth_ != nullptr) {
        (void)open(driver_, bank_, sampleRate_);
    }
}

void OplSynth::write(std::uint32_t message) {
    if (synth_ != nullptr) {
        synth_->midi_write(message);
    }
}

void OplSynth::render(float* out, std::size_t frames) {
    if (frames == 0) {
        return;
    }
    if (synth_ == nullptr) {
        std::fill_n(out, frames * 2, 0.0F);
        return;
    }
    // midi_generate takes a frame count in an unsigned int and writes stereo
    // pairs of 16-bit samples. Chunked for two reasons: that count is narrower
    // than the std::size_t a caller may hold, and the chunk is also the scratch
    // the chip's output is widened from.
    constexpr std::size_t kMaxChunk = 4096;
    pcm_.resize(kMaxChunk * 2);
    while (frames > 0) {
        const std::size_t todo = std::min(frames, kMaxChunk);
        synth_->midi_generate(pcm_.data(), static_cast<unsigned>(todo));
        widen(pcm_.data(), out, todo * 2);
        out += todo * 2;
        frames -= todo;
    }
}

unsigned OplSynth::bankCount(OplDriver driver) {
    // The counts are the drivers' own, and the only way to read them is to make
    // one -- midi_bank_count is a virtual on the instance. Cheap: the
    // constructor allocates the object and nothing else; the chip and its
    // resampler are not built until midi_init.
    const std::unique_ptr<midisynth> synth{create(driver)};
    return synth ? synth->midi_bank_count() : 0;
}

}  // namespace xpcog::codecs
