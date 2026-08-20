// AHX and Hively tracker modules, through the HivelyPlayer replayer.
//
// Port of Cog Plugins/Hively (HVLDecoder, HVLContainer, HVLMetadataReader) on
// vendor/hively. Two formats, one replayer: `.ahx` is Abyss' Highest
// eXperience, a 1994 Amiga tracker that synthesises its own waveforms instead
// of playing samples, and `.hvl` is Hively Tracker, its successor. hvl_LoadTune
// reads both and tells them apart by content -- "THX" with a version below 3,
// or "HVL" with a version below 2 -- which matters more here than it does in
// Cog; see the note on `.ahx` below.
//
// Like every synthesised format this has no natural length: the tune plays
// until it decides it has ended, and most of them never do. The length is
// therefore invented, and inventing it means *playing the whole tune* at open
// time -- see measurePlayingTime().
//
// Three things differ from Cog, each for a reason:
//
//   * **`.ahx` is claimed by vgmstream too**, for CRI's entirely unrelated
//     ADX-family format. Cog has no such conflict because it gives vgmstream a
//     lower priority; here both sit at the default, and the question was left
//     open in docs/MIDI.md. It settles itself: vgmstream is offered the file
//     first, sniffs it, declines, and the registry moves on -- exactly how
//     `.mus` was settled between MIDI and SID. Nothing needs a priority, and
//     giving this one would be the fragile answer to a question content
//     already decides.
//
//   * **Float output rather than a scaled integer copy.** The replayer renders
//     int32 and Cog converts to float by dividing by 2^24, through vDSP. Doing
//     the same conversion here means the decoder emits F32 directly rather than
//     narrowing to S16 on the way out, which is what the rest of this tree's
//     synth decoders do only because their libraries hand back S16.
//
//   * **The frame count per replayer tick is computed, not rounded.**
//     hvl_DecodeFrame renders `(freq / 50 / speedMultiplier) * speedMultiplier`
//     frames -- integer division, twice -- and Cog reads back `ceil(freq / 50)`
//     of them. Those agree for speed multipliers 1, 2 and 3 at 44,100 Hz and
//     disagree by two frames at 4, where the tail is whatever the buffer held
//     last tick. Small, and audible only as a faint buzz on the few tunes that
//     ask for the fastest timing, but there is no reason to copy it.

#include "common/SourceBytes.hpp"
#include "common/TextEncoding.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

extern "C" {
#include "hvl_replay.h"
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr std::string_view kExtensionList[] = {"hvl", "ahx"};
constexpr std::span<const std::string_view> kExtensions{kExtensionList};

constexpr std::uint32_t kChannels = 2;

/// What Cog falls back to when synthSampleRate is unset or out of range, and
/// the clamp it applies.
constexpr int kDefaultRate = 44100;
constexpr int kMinRate     = 8000;
constexpr int kMaxRate     = 192000;

/// The replayer's tick rate: a PAL Amiga's vertical blank. Everything about
/// this format's timing is counted in these, including ht_PlayingTime.
constexpr int kTicksPerSecond = 50;

/// Cog's clamp on synthDefaultLoopCount, and the same one the GME decoder uses.
constexpr int kMaxLoopCount = 10;

/// The replayer renders int32 whose full scale is 1 << 24, not 1 << 31 -- the
/// Amiga's Paula is an 8-bit DAC and the extra bits are mixing headroom.
constexpr float kFullScale = 16777216.0F;

/// Cog's `[[url fragment] intValue]`, which answers 0 for anything unparseable.
[[nodiscard]] std::uint32_t subsongFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    std::uint32_t          value    = 0;
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return fragment.empty() ? 0 : value;
}

[[nodiscard]] int preferredRate(const Settings* settings) {
    if (settings == nullptr) {
        return kDefaultRate;
    }
    const int rate = settings->SynthSampleRate();
    return (rate < kMinRate || rate > kMaxRate) ? kDefaultRate : rate;
}

/// The replayer is a C library with process-wide tables -- panning curves and
/// the waveform bank -- built once. hvl_InitReplayer() is not idempotent and
/// not thread-safe, and a decoder is constructed on whichever thread the engine
/// happens to be feeding from, so it is done here rather than at each open.
void ensureReplayerReady() {
    [[maybe_unused]] static const bool once = [] {
        hvl_InitReplayer();
        return true;
    }();
}

struct TuneDeleter {
    void operator()(hvl_tune* tune) const noexcept {
        if (tune != nullptr) {
            hvl_FreeTune(tune);
        }
    }
};
using TunePtr = std::unique_ptr<hvl_tune, TuneDeleter>;

[[nodiscard]] TunePtr loadTune(std::span<const std::byte> data, int rate) {
    ensureReplayerReady();
    if (data.empty() || data.size() > 0xFFFFFFFFU) {
        return {};
    }
    // defstereo 2 is Cog's, and is the replayer's own middle setting: 0 is hard
    // Amiga panning and 4 is mono.
    return TunePtr{hvl_LoadTune(reinterpret_cast<const std::uint8_t*>(data.data()),
                                static_cast<std::uint32_t>(data.size()),
                                static_cast<std::uint32_t>(rate), 2)};
}

/// The subsongs that actually play something the main song does not.
///
/// A subsong is a starting index into the position list, held in ht_Subsongs,
/// and ht_SubsongNr is how many the header claims. The two agree far less often
/// than the format's authors presumably intended: the replayer clamps any entry
/// pointing past the end of the position list to 0 (hvl_replay.c, "Subsongs"),
/// and a great many files in the wild declare subsongs whose entries do exactly
/// that. Every one of those starts the tune where the main song starts, and
/// therefore plays the main song.
///
/// Measured on 826 AHX and HVL modules: 94 declare subsongs, and only 34 hold a
/// single one that begins anywhere else. Cog expands that collection into 432
/// playlist rows of which 193 are distinct -- so more than half of what it adds
/// is the same tune again under a different number. It is a faithful reading of
/// ht_SubsongNr and not much of a favour to whoever has to look at the playlist.
///
/// So the entries are read rather than counted, and each start position is
/// taken once. Position 0 is seeded because that is where the main song begins;
/// a subsong claiming it is the main song by another name.
[[nodiscard]] std::vector<std::uint32_t> distinctSubsongs(const hvl_tune& tune) {
    std::vector<std::uint32_t> starts{0};
    std::vector<std::uint32_t> subsongs;

    for (std::uint32_t i = 0; i < tune.ht_SubsongNr; ++i) {
        const auto start = static_cast<std::uint32_t>(tune.ht_Subsongs[i]);
        if (std::find(starts.begin(), starts.end(), start) != starts.end()) {
            continue;
        }
        starts.push_back(start);
        // The replayer numbers subsongs from one: hvl_InitSubsong(nr) reads
        // ht_Subsongs[nr - 1], and nr 0 is the main song.
        subsongs.push_back(i + 1);
    }
    return subsongs;
}

/// Which of the two formats these bytes are.
///
/// Asked of the bytes rather than of the tune, because hvl_LoadTune answers the
/// same struct either way: it dispatches to hvl_load_ahx() or the HVL path on
/// exactly this test and then records nothing about which one ran. The header
/// is four bytes and the check is the replayer's own, so re-reading it here is
/// cheaper than carrying a patch upstream.
[[nodiscard]] bool looksLikeHvl(std::span<const std::byte> data) {
    return data.size() >= 4 && static_cast<char>(data[0]) == 'H' &&
           static_cast<char>(data[1]) == 'V' && static_cast<char>(data[2]) == 'L';
}

/// Frames hvl_DecodeFrame writes per call, which is not `rate / 50`.
///
/// It loops `speedMultiplier` times over `rate / 50 / speedMultiplier` frames,
/// and both divisions truncate -- so at 44,100 Hz a multiplier of 4 renders 880
/// frames where the obvious arithmetic says 882. Reading back the obvious
/// number replays two frames of the previous tick. Derived from the replayer's
/// own expression rather than rounded to match it.
[[nodiscard]] std::size_t framesPerTick(const hvl_tune& tune) {
    const auto multiplier = static_cast<std::uint32_t>(tune.ht_SpeedMultiplier);
    if (multiplier == 0) {
        return 0;
    }
    return static_cast<std::size_t>(tune.ht_Frequency / kTicksPerSecond / multiplier) *
           multiplier;
}

/// How many ticks the tune runs for, by playing it and counting.
///
/// There is no cheaper way: the length of a tracker module is a property of
/// where its position list jumps to, which is a property of running it. So the
/// tune is stepped through `passes` times with hvl_play_irq(), which advances
/// the sequencer without mixing any audio, and ht_PlayingTime is read off
/// afterwards. The caller must re-initialise the subsong; this leaves the
/// replayer wherever the count ended.
///
/// The safety bound is Cog's and is worth keeping: a tune whose position list
/// loops without ever setting ht_SongEndReached would otherwise spin here for
/// ever, at open time, on the thread that opened it.
[[nodiscard]] std::uint32_t measurePlayingTime(hvl_tune& tune, int passes) {
    auto remaining = static_cast<std::uint64_t>(2 * 60 * 60) *
                     static_cast<std::uint64_t>(kTicksPerSecond) *
                     static_cast<std::uint64_t>(tune.ht_SpeedMultiplier);

    for (int pass = 0; pass < passes && remaining > 0; ++pass) {
        while (tune.ht_SongEndReached == 0 && remaining > 0) {
            hvl_play_irq(&tune);
            --remaining;
        }
        tune.ht_SongEndReached = 0;
    }
    return tune.ht_PlayingTime;
}

/// Cog's title: ht_Name, trimmed. Nothing else is in an AHX header -- no
/// artist, no date, no comment -- so a one-key map is the whole of it.
[[nodiscard]] std::string tuneTitle(const hvl_tune& tune) {
    // ht_Name is a fixed 128-byte field and is not guaranteed terminated.
    const auto* begin = static_cast<const char*>(tune.ht_Name);
    const auto* end   = static_cast<const char*>(
        std::memchr(begin, '\0', sizeof(tune.ht_Name)));
    std::string name{begin, end != nullptr ? static_cast<std::size_t>(end - begin)
                                           : sizeof(tune.ht_Name)};

    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
    name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
    return codecs::toUtf8(std::move(name));
}

class HivelyDecoder final : public IDecoder {
public:
    ~HivelyDecoder() override { HivelyDecoder::close(); }

    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr) {
            return false;
        }

        const auto data = codecs::readAllBytes(*source);
        if (!data) {
            return false;
        }

        rate_ = preferredRate(settings_);
        tune_ = loadTune(*data, rate_);
        if (!tune_) {
            return false;
        }
        isHvl_       = looksLikeHvl(*data);
        hasSubsongs_ = !distinctSubsongs(*tune_).empty();

        subsong_ = subsongFromFragment(source->url());
        if (subsong_ > tune_->ht_SubsongNr) {
            subsong_ = 0;
        }

        ticksPerFrameGroup_ = framesPerTick(*tune_);
        if (ticksPerFrameGroup_ == 0) {
            tune_.reset();
            return false;
        }

        // Length first, because measuring it means playing the tune -- so the
        // subsong has to be initialised twice: once for the measurement and
        // once to put the replayer back at the top for the listener.
        hvl_InitSubsong(tune_.get(), subsong_);
        const std::uint32_t ticks = measurePlayingTime(*tune_, loopPasses());
        hvl_InitSubsong(tune_.get(), subsong_);

        // ht_PlayingTime counts replayer ticks, of which there are 50 per
        // second times the speed multiplier -- so a tune at multiplier 4 gets
        // four times as many ticks for the same wall-clock second.
        const double seconds =
            static_cast<double>(ticks) /
            (static_cast<double>(kTicksPerSecond) *
             static_cast<double>(tune_->ht_SpeedMultiplier));

        bodyFrames_ = static_cast<std::int64_t>(seconds * static_cast<double>(rate_));
        fadeFrames_ = fadeFrameCount();
        totalFrames_ = bodyFrames_ + fadeFrames_;

        title_ = tuneTitle(*tune_);

        format_.sampleRate    = static_cast<double>(rate_);
        format_.channels      = kChannels;
        format_.channelConfig = 0x3;  // FL | FR
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;

        tick_.assign(ticksPerFrameGroup_ * kChannels, 0);
        pending_.clear();
        pendingRead_ = 0;
        framePos_    = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = isHvl_ ? "Hively Tracker" : "Abyss' Highest eXperience";
        props.encoding    = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override {
        MetadataMap tags;
        if (!title_.empty()) {
            tags.set("title", title_);
        }
        // Only when the file actually holds more than one playable song, and by
        // the same rule the container expands on -- a tune numbered "1 of 1" is
        // noise in a playlist, and so is one numbered against subsongs that all
        // turn out to be the main song.
        if (hasSubsongs_) {
            tags.set("track", std::to_string(subsong_ + 1));
        }
        return tags;
    }

    bool readAudio(AudioChunk& out) override {
        // Asked per read rather than latched at open: the listener can switch
        // repeat-one on part-way through a tune and expects the fade to stop
        // coming.
        const bool endless = loopForever(settings_);
        if (!tune_ || (!endless && framePos_ >= totalFrames_)) {
            return false;
        }

        const std::size_t want =
            endless ? kFramesPerRead
                    : static_cast<std::size_t>(
                          std::min<std::int64_t>(static_cast<std::int64_t>(kFramesPerRead),
                                                 totalFrames_ - framePos_));
        if (want == 0) {
            return false;
        }

        pending_.resize(want * kChannels);
        const std::size_t got = render(pending_.data(), want);
        if (got == 0) {
            return false;
        }

        applyFade(pending_.data(), got, endless);

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / static_cast<double>(rate_);
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(got);
        std::memcpy(dst, pending_.data(), got * kChannels * sizeof(float));
        out.setFrameCount(got);

        framePos_ += static_cast<std::int64_t>(got);
        return true;
    }

    /// Seeking is replaying. The replayer has no way to be placed at a position
    /// -- a tracker's state at any moment is the accumulation of every note and
    /// effect before it -- so going backwards means starting the subsong again,
    /// and going forwards means rendering the gap and throwing it away.
    ///
    /// Cog steps with hvl_play_irq() alone, which advances the sequencer but
    /// never mixes, so its filters and envelopes arrive at the seek point with
    /// no history and the first moment after a seek is wrong. Rendering into a
    /// discard buffer costs the mixing but lands in the state the tune would
    /// actually be in.
    std::int64_t seek(std::int64_t frame) override {
        if (!tune_) {
            return -1;
        }
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        if (frame < framePos_) {
            hvl_InitSubsong(tune_.get(), subsong_);
            framePos_    = 0;
            pendingRead_ = 0;
        }

        while (framePos_ < frame) {
            const auto step = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(kFramesPerRead),
                                       frame - framePos_));
            discard_.resize(step * kChannels);
            const std::size_t got = render(discard_.data(), step);
            if (got == 0) {
                break;
            }
            framePos_ += static_cast<std::int64_t>(got);
        }
        return framePos_;
    }

    void close() override {
        tune_.reset();
        pending_.clear();
        discard_.clear();
        tick_.clear();
        pendingRead_ = 0;
        framePos_    = 0;
    }

private:
    static constexpr std::size_t kFramesPerRead = 1024;

    /// How many times through the tune its stated length is. Cog hardcodes two;
    /// this is the same setting the other synth decoders read, so someone who
    /// wants three passes of a favourite tune can say so.
    [[nodiscard]] int loopPasses() const {
        const int loops =
            (settings_ != nullptr) ? settings_->SynthDefaultLoopCount() : 2;
        // At least one: zero passes is a tune of no length, which is not a
        // preference anybody means by "do not loop".
        return std::clamp(loops, 1, kMaxLoopCount);
    }

    [[nodiscard]] std::int64_t fadeFrameCount() const {
        const double seconds =
            (settings_ != nullptr) ? settings_->SynthDefaultFadeSeconds() : 8.0;
        return static_cast<std::int64_t>(
            std::ceil(std::max(0.0, seconds) * static_cast<double>(rate_)));
    }

    /// Fills `out` with `frames` frames, pulling whole replayer ticks and
    /// carrying the remainder of the last one across calls.
    ///
    /// The carry is what makes this correct rather than merely close:
    /// hvl_DecodeFrame renders a fixed 882-odd frames and nothing asks for a
    /// multiple of that, so without somewhere to keep the tail every read would
    /// either drop it or re-render it.
    [[nodiscard]] std::size_t render(float* out, std::size_t frames) {
        std::size_t filled = 0;
        while (filled < frames) {
            if (pendingRead_ == 0) {
                // Left and right are handed in as two pointers into the same
                // buffer, one int32 apart, with a stride of two -- which is how
                // the replayer writes interleaved stereo.
                auto* base = reinterpret_cast<std::int8_t*>(tick_.data());
                hvl_DecodeFrame(tune_.get(), base,
                                base + sizeof(std::int32_t),
                                static_cast<std::int32_t>(sizeof(std::int32_t) *
                                                          kChannels));
                pendingRead_ = ticksPerFrameGroup_;
            }

            const std::size_t take = std::min(frames - filled, pendingRead_);
            const std::size_t from = (ticksPerFrameGroup_ - pendingRead_) * kChannels;
            for (std::size_t i = 0; i < take * kChannels; ++i) {
                out[filled * kChannels + i] =
                    static_cast<float>(tick_[from + i]) / kFullScale;
            }
            filled += take;
            pendingRead_ -= take;
        }
        return filled;
    }

    /// Ramps the tail to silence, which is what turns a tune that never ends
    /// into a track with a length. Not applied while looping for ever: that is
    /// the listener saying they did not want the tune to end.
    void applyFade(float* frames, std::size_t count, bool endless) const {
        if (endless || fadeFrames_ <= 0 ||
            framePos_ + static_cast<std::int64_t>(count) <= bodyFrames_) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::int64_t position = framePos_ + static_cast<std::int64_t>(i);
            if (position <= bodyFrames_) {
                continue;
            }
            const auto gain = static_cast<float>(
                std::max(0.0, static_cast<double>(totalFrames_ - position) /
                                  static_cast<double>(fadeFrames_)));
            for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
                frames[i * kChannels + channel] *= gain;
            }
        }
    }

    const Settings* settings_ = nullptr;

    TunePtr     tune_;
    AudioFormat format_{};

    int           rate_        = kDefaultRate;
    std::uint32_t subsong_     = 0;
    bool          isHvl_       = false;
    bool          hasSubsongs_ = false;
    std::string   title_;

    std::int64_t bodyFrames_  = 0;
    std::int64_t fadeFrames_  = 0;
    std::int64_t totalFrames_ = 0;
    std::int64_t framePos_    = 0;

    /// One replayer tick, interleaved stereo int32, and how much of it is still
    /// unread.
    std::vector<std::int32_t> tick_;
    std::size_t               ticksPerFrameGroup_ = 0;
    std::size_t               pendingRead_        = 0;

    std::vector<float> pending_;
    std::vector<float> discard_;
};

/// A tune with real subsongs expands to one URL per song, addressed by
/// fragment; anything else stays a single row. See distinctSubsongs() for what
/// "real" means here and why counting ht_SubsongNr is not it.
std::vector<Url> expandSubsongs(const Url& url, ISource& source,
                                const PluginRegistry& /*registry*/) {
    if (!url.fragment().empty()) {
        return {url};
    }

    const auto data = codecs::readAllBytes(source);
    if (!data) {
        return {url};
    }

    // Any rate will do: nothing here is rendered, and which songs a file holds
    // is a property of the file rather than of playback.
    const TunePtr tune = loadTune(*data, kDefaultRate);
    if (!tune) {
        return {url};
    }

    const std::vector<std::uint32_t> subsongs = distinctSubsongs(*tune);
    if (subsongs.empty()) {
        return {url};
    }

    std::vector<Url> tracks;
    tracks.reserve(subsongs.size() + 1);
    tracks.push_back(url.withFragment("0"));
    for (const std::uint32_t subsong : subsongs) {
        tracks.push_back(url.withFragment(std::to_string(subsong)));
    }
    return tracks;
}

}  // namespace
}  // namespace xpcog

void xpcog_register_hively(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "HivelyContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .expand     = &xpcog::expandSubsongs,
    });

    r.addDecoder({
        .name       = "HivelyDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::HivelyDecoder>();
        },
        .available = nullptr,
    });
}
