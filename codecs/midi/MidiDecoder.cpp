// MIDI playback: the sequencer in MidiFile driving a synthesiser.
//
// Port of Cog Plugins/MIDI/MIDIDecoder.mm and the callback half of
// MIDIPlayer.cpp. The synthesiser is Nuked OPL3 (OplSynth.hpp), which is stage
// 1 of docs/MIDI.md and the first of three: SpessaSynth and Nuked SC-55 slot in
// behind the same event stream, and neither needs anything here to change shape.
//
// ---------------------------------------------------------------------------
// Why a MIDI file has a length at all
// ---------------------------------------------------------------------------
// It does not state one. What it has is a last event, and a synthesiser still
// ringing after it -- so "the end" is already a judgement. Worse, plenty of
// these files are game music and repeat: XMI carries loop controllers, and RPG
// Maker and Touhou files mark a loop with text. For those the last event is not
// the end of the music at all, it is the end of one pass.
//
// So, as Cog does: a file that states a loop is played intro-then-loop, the
// loop repeated `synthDefaultLoopCount` times, and faded. A file that states
// none is played to its last event and stops, with no fade, because that really
// is where the score ends. And with repeat-one set the listener has overruled
// the whole calculation, so a looping file simply never stops -- see
// LoopPolicy.hpp.
//
// ---------------------------------------------------------------------------
// The event loop
// ---------------------------------------------------------------------------
// Events are delivered at exact sample positions rather than on a fixed grid.
// Cog's MIDIPlayer quantises to a chunk (256 frames for MSPlayer) because its
// backends want to be driven that way; nothing here does, and the OPL chip is
// happy to be asked for four samples. Doing it exactly costs nothing and means
// a fast run of notes is not smeared onto chunk boundaries.
//
// What is *not* exact is the chip's own 50 ms register-write queue
// (opl3class.cpp: `lat`), which delays every write by that much. It applies
// uniformly, so it is a constant latency rather than jitter, and Cog has it
// too -- it is the queue that lets the emulation batch writes without losing
// their order.

#include "common/SourceBytes.hpp"
#include "midi/MidiFile.hpp"
#include "midi/MidiSynth.hpp"
#include "midi/OplSynth.hpp"
#include "midi/Sc55Roms.hpp"
#include "midi/Sc55Synth.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr std::size_t kFramesPerRead = 1024;

/// Shared with the other synthesised formats; see settings.def.
constexpr double kDefaultSampleRate = 44100.0;
constexpr double kMinSampleRate     = 8000.0;
constexpr double kMaxSampleRate     = 192000.0;

/// Cog's clamp on synthDefaultLoopCount.
constexpr int kMaxLoopCount = 10;

/// What `midiPlugin` names, resolved.
struct SynthChoice {
    enum class Backend : std::uint8_t { Opl, Sc55 };

    Backend           backend = Backend::Opl;
    codecs::OplDriver driver  = codecs::OplDriver::Doom;
    unsigned          bank    = 0;
};

/// Reads the setting's `DOOM<n>` / `OPL3W<n>` / `NukeSc55` vocabulary.
///
/// Anything else -- an AudioUnit component code from a macOS Cog, or a synth
/// whose stage has not landed -- falls back to the default rather than refusing
/// the file. A player that will not open a `.mid` because a settings string
/// names something it has never heard of is worse than one that plays it on the
/// synthesiser it does have.
[[nodiscard]] SynthChoice parseSynthChoice(std::string_view setting) {
    const auto starts = [setting](std::string_view prefix) {
        return setting.size() >= prefix.size() &&
               setting.substr(0, prefix.size()) == prefix;
    };

    SynthChoice      choice;
    std::string_view rest;
    if (setting == "NukeSc55") {
        choice.backend = SynthChoice::Backend::Sc55;
        return choice;
    }
    // OPL3W before DOOM: neither is a prefix of the other, but testing the
    // longer name first is the habit that keeps that from mattering.
    if (starts("OPL3W")) {
        choice.driver = codecs::OplDriver::GeneralMidi;
        rest          = setting.substr(5);
    } else if (starts("DOOM")) {
        choice.driver = codecs::OplDriver::Doom;
        rest          = setting.substr(4);
    } else {
        return choice;
    }

    unsigned bank = 0;
    for (const char c : rest) {
        if (c < '0' || c > '9') {
            return choice;  // trailing rubbish: the bank stays at the default
        }
        bank = bank * 10 + static_cast<unsigned>(c - '0');
        if (bank > 999) {
            return choice;
        }
    }
    choice.bank = bank;
    return choice;
}

/// Subsongs are numbered from zero here, as they are in Cog
/// (`[[[s url] fragment] intValue]`, which is 0 for an absent fragment) and
/// unlike SID, whose library numbers songs from one.
[[nodiscard]] std::size_t subsongFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    std::size_t            value    = 0;
    if (fragment.empty()) {
        return 0;
    }
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    return value;
}

/// Reads a source whole and parses it. A MIDI file is small -- the largest in a
/// 197,000-file corpus is under 4 MB, the average 19 KB -- and every processor
/// midi_processing has wants all of it at once, so there is no streaming path
/// to design around.
[[nodiscard]] bool loadFile(ISource& source, codecs::MidiFile& out) {
    const auto bytes = codecs::readAllBytes(source);
    if (!bytes || bytes->empty()) {
        return false;
    }
    // The library takes a vector of unsigned char and the reader gives one of
    // std::byte, which is a distinct type however identical the storage. One
    // copy of at most a few megabytes, rather than a second reader that differs
    // from the shared one only in its element type.
    const std::vector<std::uint8_t> raw(
        reinterpret_cast<const std::uint8_t*>(bytes->data()),
        reinterpret_cast<const std::uint8_t*>(bytes->data()) + bytes->size());
    return out.parse(raw, source.url().extension());
}

class MidiDecoder final : public IDecoder {
public:
    ~MidiDecoder() override { MidiDecoder::close(); }

    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr) {
            return false;
        }
        if (!loadFile(*source, file_)) {
            return false;
        }

        subsong_ = subsongFromFragment(source->url());
        if (subsong_ >= file_.subsongCount()) {
            return false;
        }

        choice_ = parseSynthChoice(
            settings_ != nullptr ? settings_->MidiPlugin() : std::string{"DOOM0"});

        // The SC-55's ROMs are read once and kept, not re-read on every seek:
        // going backwards means booting a fresh machine, and that should not
        // also mean hashing 3.6 MB off disk again.
        roms_.reset();
        if (choice_.backend == SynthChoice::Backend::Sc55 && settings_ != nullptr) {
            roms_ = codecs::loadSc55Roms(
                std::filesystem::path{settings_->MidiRomPath()});
        }

        if (!buildSynth()) {
            return false;
        }
        // Taken from whichever synthesiser was actually built. For the OPL it is
        // the setting; for the SC-55 it is the hardware's, which is neither
        // 44100 nor negotiable.
        sampleRate_ = synth_->sampleRate();

        stream_ = file_.stream(subsong_, sampleRate_);
        tags_   = file_.metadata(subsong_);

        computeLength();

        format_.sampleRate    = sampleRate_;
        format_.channels      = 2;
        format_.channelConfig = 0x3;  // FL|FR
        format_.format        = SampleFormat::S16;
        format_.bitsPerSample = 16;

        eventIndex_ = 0;
        seqSample_  = 0;
        framePos_   = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = "MIDI";
        // Which synthesiser actually ran, not which one was asked for -- a
        // configured SC-55 with no ROMs plays on the OPL, and this is where
        // that shows.
        props.encoding    = (synth_ != nullptr) ? synth_->displayName() : "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

    bool readAudio(AudioChunk& out) override {
        // Asked per read rather than latched at open: the listener can switch
        // repeat-one on part-way through and expects the fade to stop coming.
        const bool endless = looped_ && loopForever(settings_);
        if (synth_ == nullptr || (!endless && framePos_ >= totalFrames_)) {
            return false;
        }

        const auto want =
            endless ? kFramesPerRead
                    : static_cast<std::size_t>(std::min<std::int64_t>(
                          static_cast<std::int64_t>(kFramesPerRead),
                          totalFrames_ - framePos_));
        if (want == 0) {
            return false;
        }

        scratch_.resize(want * 2);
        render(scratch_.data(), want);
        applyFade(scratch_.data(), want);

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / sampleRate_;
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(want);
        std::memcpy(dst, scratch_.data(), want * 2 * sizeof(std::int16_t));
        out.setFrameCount(want);

        framePos_ += static_cast<std::int64_t>(want);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);
        if (synth_ == nullptr) {
            return -1;
        }

        // An FM chip is a state machine with envelopes in flight; there is no
        // way to compute where it would have been. Going backwards means
        // resetting it and playing the sequence again, and going forwards means
        // rendering the gap and throwing the audio away -- which is also why
        // dispatching the events without rendering would not do, since the
        // envelopes advance with the samples and not with the events.
        if (frame < framePos_) {
            if (!restart()) {
                return -1;
            }
        }

        while (framePos_ < frame) {
            const auto step = static_cast<std::size_t>(std::min<std::int64_t>(
                static_cast<std::int64_t>(kFramesPerRead), frame - framePos_));
            scratch_.resize(step * 2);
            render(scratch_.data(), step);
            framePos_ += static_cast<std::int64_t>(step);
        }
        return framePos_;
    }

    void close() override {
        synth_.reset();
        stream_ = codecs::MidiStream{};
        tags_.clear();
    }

private:
    /// Constructs the synthesiser the setting asked for, or the one that can
    /// actually run.
    ///
    /// A configured SC-55 with no ROMs falls back to the OPL rather than
    /// refusing the file, which is the same call parseSynthChoice makes about a
    /// name it does not recognise. Refusing would mean a `.mid` that will not
    /// open at all until five files nobody can be pointed at are found -- and
    /// the fallback is visible, since properties() reports what actually ran.
    bool buildSynth() {
        synth_.reset();

        if (choice_.backend == SynthChoice::Backend::Sc55 && roms_) {
            auto sc55 = std::make_unique<codecs::Sc55Synth>();
            if (sc55->open(*roms_)) {
                synth_ = std::move(sc55);
                return true;
            }
        }

        double rate = kDefaultSampleRate;
        if (settings_ != nullptr) {
            const auto configured = static_cast<double>(settings_->SynthSampleRate());
            if (configured >= kMinSampleRate && configured <= kMaxSampleRate) {
                rate = configured;
            }
        }
        auto opl = std::make_unique<codecs::OplSynth>();
        if (!opl->open(choice_.driver, choice_.bank, rate)) {
            return false;
        }
        synth_ = std::move(opl);
        return true;
    }

    bool restart() {
        if (!buildSynth()) {
            return false;
        }
        eventIndex_ = 0;
        seqSample_  = 0;
        framePos_   = 0;
        return true;
    }

    void computeLength() {
        const double           natural = file_.duration(subsong_);
        const codecs::MidiLoop lp      = file_.loop(subsong_);

        loopStartSample_ = 0;
        loopEndSample_   = 0;
        loopStartIndex_  = 0;

        // A loop that spans the whole sequence is not a loop -- it says only
        // "repeat from the start", which is what a player already does when the
        // listener asks for it. Cog draws the line in exactly this place.
        looped_ = lp.valid && (lp.start != 0.0 || lp.end != natural);

        double lengthSeconds = natural;
        double fadeSeconds   = 0.0;
        if (looped_) {
            int count = (settings_ != nullptr) ? settings_->SynthDefaultLoopCount() : 2;
            count     = std::clamp(count, 0, kMaxLoopCount);
            lengthSeconds = lp.start + (lp.end - lp.start) * count;
            fadeSeconds =
                (settings_ != nullptr) ? settings_->SynthDefaultFadeSeconds() : 8.0;
            fadeSeconds = std::max(0.0, fadeSeconds);

            loopStartSample_ = toSamples(lp.start);
            loopEndSample_   = toSamples(lp.end);
            loopStartIndex_  = (stream_.loopStart != codecs::MidiStream::kNoLoop)
                                   ? stream_.loopStart
                                   : 0;
            // Nothing to repeat, so nothing to rewind into: a zero-length or
            // inverted loop would otherwise spin here for ever producing no
            // samples, which is a hang and not a quiet passage.
            if (loopEndSample_ <= loopStartSample_) {
                looped_       = false;
                lengthSeconds = natural;
                fadeSeconds   = 0.0;
            }
        }

        fadeFrames_  = static_cast<std::int64_t>(std::llround(fadeSeconds * sampleRate_));
        totalFrames_ = static_cast<std::int64_t>(std::llround(lengthSeconds * sampleRate_)) +
                       fadeFrames_;
    }

    [[nodiscard]] std::uint64_t toSamples(double seconds) const {
        return static_cast<std::uint64_t>(std::max(0.0, seconds) * sampleRate_);
    }

    /// Fills `frames` stereo frames, delivering every event that falls inside
    /// them at its own sample and rewinding at the loop point.
    void render(std::int16_t* out, std::size_t frames) {
        std::size_t produced = 0;
        while (produced < frames) {
            if (looped_ && seqSample_ >= loopEndSample_) {
                seqSample_  = loopStartSample_;
                eventIndex_ = loopStartIndex_;
            }

            while (eventIndex_ < stream_.events.size() &&
                   stream_.events[eventIndex_].timestampSamples <= seqSample_) {
                dispatch(stream_.events[eventIndex_]);
                ++eventIndex_;
            }

            std::uint64_t run = frames - produced;
            if (eventIndex_ < stream_.events.size()) {
                run = std::min(run, stream_.events[eventIndex_].timestampSamples -
                                        seqSample_);
            }
            if (looped_) {
                run = std::min(run, loopEndSample_ - seqSample_);
            }
            // Both bounds are strictly ahead of seqSample_ by the two blocks
            // above, so this cannot be zero; the guard is here because a wrong
            // change to either would otherwise hang rather than fail a test.
            if (run == 0) {
                run = frames - produced;
            }

            synth_->render(out + produced * 2, static_cast<std::size_t>(run));
            produced += static_cast<std::size_t>(run);
            seqSample_ += run;
        }
    }

    void dispatch(const codecs::MidiStreamEvent& event) {
        // One synthesiser, so one port. A file naming a second wants a second
        // machine, which is what Cog builds for it and what this does not.
        if (event.port != 0) {
            return;
        }
        if (!event.isSysex) {
            synth_->write(event.message);
            return;
        }
        // A synthesiser with nowhere to put a SysEx ignores it; the OPL is one.
        if (event.message < stream_.sysex.size()) {
            const codecs::MidiSysex& sysex = stream_.sysex[event.message];
            if (sysex.port == 0 && !sysex.data.empty()) {
                synth_->writeSysex(sysex.data);
            }
        }
    }

    void applyFade(std::int16_t* frames, std::size_t count) {
        // No fade while looping for ever: the fade is what turns a sequence
        // that repeats into a track that ends.
        if (fadeFrames_ <= 0 || (looped_ && loopForever(settings_))) {
            return;
        }
        const std::int64_t fadeStart = totalFrames_ - fadeFrames_;
        if (framePos_ + static_cast<std::int64_t>(count) <= fadeStart) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::int64_t position = framePos_ + static_cast<std::int64_t>(i);
            if (position <= fadeStart) {
                continue;
            }
            const double gain = static_cast<double>(totalFrames_ - position) /
                                static_cast<double>(fadeFrames_);
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const double scaled =
                    static_cast<double>(frames[i * 2 + channel]) * gain;
                frames[i * 2 + channel] =
                    static_cast<std::int16_t>(std::clamp(scaled, -32768.0, 32767.0));
            }
        }
    }

    const Settings* settings_ = nullptr;

    codecs::MidiFile                  file_;
    codecs::MidiStream                stream_;
    std::unique_ptr<codecs::MidiSynth> synth_;

    SynthChoice                       choice_;
    /// Kept for the lifetime of the decoder so a backwards seek can boot a
    /// fresh machine without going back to disk.
    std::optional<codecs::Sc55RomSet> roms_;

    std::size_t subsong_    = 0;
    AudioFormat format_{};
    double      sampleRate_ = kDefaultSampleRate;
    MetadataMap tags_;

    /// Where the sequence is, which is not where the output is: after a rewind
    /// the two differ by however much of the loop has already been played.
    std::uint64_t seqSample_  = 0;
    std::size_t   eventIndex_ = 0;
    std::int64_t  framePos_   = 0;

    bool          looped_          = false;
    std::uint64_t loopStartSample_ = 0;
    std::uint64_t loopEndSample_   = 0;
    std::size_t   loopStartIndex_  = 0;

    std::int64_t totalFrames_ = 0;
    std::int64_t fadeFrames_  = 0;

    std::vector<std::int16_t> scratch_;
};

/// A file holding more than one sequence expands to one URL per sequence.
///
/// That is XMI above all -- id and Origin shipped a game's whole soundtrack as
/// one file -- and format-2 Standard MIDI, which is rare but is exactly the
/// same thing. Everything else has one, and is returned unchanged so a second
/// claimant on a shared extension still gets its turn.
std::vector<Url> expandFile(const Url& url, ISource& source,
                            const PluginRegistry& /*registry*/) {
    if (!url.fragment().empty()) {
        return {url};
    }

    codecs::MidiFile file;
    if (!loadFile(source, file) || file.subsongCount() <= 1) {
        return {url};
    }

    std::vector<Url> songs;
    songs.reserve(file.subsongCount());
    for (std::size_t i = 0; i < file.subsongCount(); ++i) {
        songs.push_back(url.withFragment(std::to_string(i)));
    }
    return songs;
}

/// Cog's list, from MIDIDecoder.mm. `gmf` and `syx` have processors but no
/// extension in Cog either -- they are reached by content, through a file
/// misnamed as something on this list.
///
/// Two collide. `mus` is also a Commodore 64 format and is claimed by
/// codecs/sid; `lds` is Loudness and belongs to nobody else here yet, though
/// Cog gives it to AdPlug. Both are settled by content rather than by priority:
/// midi_processing sniffs the file, a C64 `.mus` fails to parse here, and the
/// registry moves on to the next claimant.
constexpr std::string_view kExtensions[] = {
    "hmi", "hmp", "hmq", "kar",  "lds",  "mds", "mid",
    "midi", "mids", "mus", "mxmf", "rmi", "xmf", "xmi"};

}  // namespace
}  // namespace xpcog

void xpcog_register_midi(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "MidiContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .expand     = &xpcog::expandFile,
    });

    r.addDecoder({
        .name       = "MidiDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::MidiDecoder>();
        },
        .available = nullptr,
    });
}
