// Syntrax modules (.jxs), through the libjaytrax replayer in vendor/syntrax.
//
// Port of Cog Plugins/Syntrax (jxsDecoder, jxsContainer, jxsMetadataReader).
// Syntrax was Reinier van Vliet's tracker for DOS and Windows, descended from
// the Amiga's Mugician; a .jxs holds up to sixteen channels, a bank of
// instruments that are equal parts sampler and synthesiser, and any number of
// subsongs, each with its own order list, tempo and echo settings.
//
// Four things differ from Cog, each for a reason:
//
//   * **A subsong number out of range is clamped.** jaytrax_changeSubsong()
//     bounds-checks with `subsongnr > song->nrofsongs`, which lets the value
//     *equal* the count through and read one past the end of the subsong array.
//     Cog takes the number straight off the URL fragment with no check of its
//     own, so `something.jxs#4` on a three-subsong file is an out-of-bounds
//     read on a pointer array. Clamped here rather than patched upstream, so
//     the vendored replayer stays comparable to Cog's copy.
//
//   * **Seeking renders and discards.** jaytrax_renderChunk() accepts a null
//     buffer and advances the sequencer without mixing, which is what Cog seeks
//     with -- but the mixer is where a voice's sample position, its synth
//     phase, the echo delay lines and the declick overlap buffer are all
//     maintained. Skipping it arrives at the seek point with a correct pattern
//     position and none of that, so the first moments after a seek are a
//     different sound. Rendering into a discard buffer costs the mixing and
//     lands in the state the module would actually be in.
//
//   * **A song that never ends gets a length anyway.** jaytrax_getLength()
//     gives up after thirty minutes and answers -1, which Cog adds the fade to
//     and reports as a track of about eight seconds' negative length. It is
//     rare -- it takes a module whose order list loops without ever setting the
//     loop counter -- but the failure is silent and total.
//
//   * **The interpolator follows the resampler quality setting**, as it does in
//     Cog, but the settings are not the same list. Cog's `resampling` names an
//     algorithm (zoh, linear, cubic, sinc) and maps each to the replayer's
//     matching one; here it names a soxr quality tier, so the mapping is by
//     intent rather than by name. See interpolatorFor().
//
// The `.jxs` extension is claimed by nothing else here.

#include "common/SourceBytes.hpp"
#include "common/TextEncoding.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

extern "C" {
#include "jaytrax.h"
#include "jxs.h"
// ERR_OK and friends, which jxsfile_readSongMem returns. It has no include
// guard of its own -- it is four lines of enum that upstream only ever includes
// from jxs.c -- so it is included here exactly once and nowhere else.
#include "ioutil.h"
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

constexpr std::string_view kExtensionList[] = {"jxs"};
constexpr std::span<const std::string_view> kExtensions{kExtensionList};
constexpr std::string_view kMimeTypeList[] = {"audio/x-jxs"};
constexpr std::span<const std::string_view> kMimeTypes{kMimeTypeList};

constexpr std::uint32_t kChannels = 2;

/// What Cog falls back to when synthSampleRate is unset or out of range, and
/// the clamp it applies.
constexpr int kDefaultRate = 44100;
constexpr int kMinRate     = 8000;
constexpr int kMaxRate     = 192000;

/// Cog's clamp on synthDefaultLoopCount, and the same one the other synth
/// decoders here use.
constexpr int kMaxLoopCount = 10;

[[nodiscard]] int preferredRate(const Settings* settings) {
    if (settings == nullptr) {
        return kDefaultRate;
    }
    const int rate = settings->SynthSampleRate();
    return (rate < kMinRate || rate > kMaxRate) ? kDefaultRate : rate;
}

/// Which of the replayer's interpolators to resample instrument samples with.
///
/// Cog reads the same `resampling` setting the output chain uses, and its
/// values name algorithms, so its mapping is one-to-one. XPCog's name soxr
/// quality tiers instead -- there is one resampler and it has grades, not
/// kinds -- so this maps intent: the cheapest tier gets the cheapest
/// interpolator and the most expensive gets the sinc.
///
/// `high` is the default and lands on cubic, which is what Cog defaults to as
/// well; nobody's modules change how they sound by moving between the two
/// players at default settings.
[[nodiscard]] std::uint8_t interpolatorFor(const Settings* settings) {
    const std::string quality =
        (settings != nullptr) ? settings->Resampling() : std::string{"high"};
    if (quality == "quick") {
        return ITP_NEAREST;
    }
    if (quality == "low") {
        return ITP_LINEAR;
    }
    if (quality == "best") {
        return ITP_SINC;
    }
    return ITP_CUBIC;  // medium, high, and anything unrecognised
}

/// Cog's `[[url fragment] intValue]`, which answers 0 for anything unparseable.
[[nodiscard]] int subsongFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    int                    value    = 0;
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = (value * 10) + (c - '0');
        if (value > 0xFFFF) {
            return 0;  // Nothing has that many subsongs; it is not a number we mean.
        }
    }
    return fragment.empty() ? 0 : value;
}

struct SongDeleter {
    void operator()(JT1Song* song) const noexcept {
        if (song != nullptr) {
            jxsfile_freeSong(song);
        }
    }
};
struct PlayerDeleter {
    void operator()(JT1Player* player) const noexcept {
        if (player != nullptr) {
            jaytrax_free(player);
        }
    }
};
using SongPtr   = std::unique_ptr<JT1Song, SongDeleter>;
using PlayerPtr = std::unique_ptr<JT1Player, PlayerDeleter>;

[[nodiscard]] SongPtr loadSong(std::span<const std::byte> data) {
    JT1Song* song = nullptr;
    if (data.empty() ||
        jxsfile_readSongMem(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(),
                            &song) != ERR_OK) {
        return {};
    }
    return SongPtr{song};
}

/// How many subsongs the file holds, as far as anything should trust it.
///
/// nrofsongs indexes an array the loader allocated to exactly that size, so
/// unlike AHX's subsong table there is nothing here that needs checking against
/// the file's contents -- every entry is a real order list. What it does need is
/// a floor: a song claiming none has no subsong 0 to play, and jaytrax_loadSong
/// dereferences subsongs[0] unconditionally.
[[nodiscard]] int subsongCount(const JT1Song& song) {
    return song.nrofsongs > 0 ? song.nrofsongs : 0;
}

/// The subsong's name, trimmed. Cog notes that some are all spaces, which is
/// why the trim matters: an all-space title displaces the filename in a
/// playlist and shows nothing in its place.
[[nodiscard]] std::string subsongTitle(const JT1Subsong& subsong) {
    const auto* begin = static_cast<const char*>(subsong.name);
    const auto* end   = static_cast<const char*>(
        std::memchr(begin, '\0', sizeof(subsong.name)));
    std::string name{begin, end != nullptr ? static_cast<std::size_t>(end - begin)
                                           : sizeof(subsong.name)};

    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
    name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
    return codecs::toUtf8(std::move(name));
}

class SyntraxDecoder final : public IDecoder {
public:
    ~SyntraxDecoder() override { SyntraxDecoder::close(); }

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

        song_ = loadSong(*data);
        if (!song_ || subsongCount(*song_) == 0) {
            song_.reset();
            return false;
        }

        player_ = PlayerPtr{jaytrax_init()};
        if (!player_ || jaytrax_loadSong(player_.get(), song_.get()) == 0) {
            close();
            return false;
        }

        subsongs_ = subsongCount(*song_);
        subsong_  = std::clamp(subsongFromFragment(source->url()), 0, subsongs_ - 1);
        rate_     = preferredRate(settings_);

        jaytrax_setInterpolation(player_.get(), interpolatorFor(settings_));

        // Measuring the length starts the subsong and plays it through, so the
        // subsong has to be selected twice: once for the measurement and once
        // to put the replayer back at the top for the listener.
        bodyFrames_ = measureLength();
        jaytrax_changeSubsong(player_.get(), subsong_);

        // No fade on a module that ends by itself. jaytrax_getLength() stops
        // either because the loop counter reached its target or because
        // playFlg went to zero, and only the first of those is a song still
        // playing when its allotted passes ran out.
        fadeFrames_  = endsByItself_ ? 0 : fadeFrameCount();
        totalFrames_ = bodyFrames_ + fadeFrames_;

        title_ = (player_->subsong != nullptr) ? subsongTitle(*player_->subsong)
                                               : std::string{};

        format_.sampleRate    = static_cast<double>(rate_);
        format_.channels      = kChannels;
        format_.channelConfig = 0x3;  // FL | FR
        format_.format        = SampleFormat::S16;
        format_.bitsPerSample = 16;

        scratch_.assign(static_cast<std::size_t>(kFramesPerRead) * kChannels, 0);
        framePos_ = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = "Syntrax";
        props.encoding    = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override {
        MetadataMap tags;
        if (!title_.empty()) {
            tags.set("title", title_);
        }
        // Only when the file actually holds more than one subsong: a module
        // numbered "1 of 1" is noise in a playlist.
        if (subsongs_ > 1) {
            tags.set("track", std::to_string(subsong_ + 1));
            tags.set("totaltracks", std::to_string(subsongs_));
        }
        return tags;
    }

    bool readAudio(AudioChunk& out) override {
        // Asked per read rather than latched at open: the listener can switch
        // repeat-one on part-way through a module and expects the fade to stop
        // coming.
        const bool endless = loopForever(settings_);
        if (!player_ || (!endless && framePos_ >= totalFrames_)) {
            return false;
        }
        // playFlg goes to zero when a non-looping module reaches its end. Past
        // that point the replayer emits silence for ever, which is not a track.
        if (player_->playFlg == 0) {
            return false;
        }

        const auto want = static_cast<std::size_t>(
            endless ? kFramesPerRead
                    : std::min<std::int64_t>(kFramesPerRead, totalFrames_ - framePos_));
        if (want == 0) {
            return false;
        }

        render(scratch_.data(), want);
        applyFade(scratch_.data(), want, endless);

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / static_cast<double>(rate_);
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(want);
        std::memcpy(dst, scratch_.data(), want * kChannels * sizeof(std::int16_t));
        out.setFrameCount(want);

        framePos_ += static_cast<std::int64_t>(want);
        return true;
    }

    /// Seeking is replaying. A tracker's state at any moment is the
    /// accumulation of every note and effect before it, so going backwards
    /// means starting the subsong again and going forwards means rendering the
    /// gap and throwing it away.
    std::int64_t seek(std::int64_t frame) override {
        if (!player_) {
            return -1;
        }
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        if (frame < framePos_) {
            jaytrax_changeSubsong(player_.get(), subsong_);
            framePos_ = 0;
        }

        while (framePos_ < frame && player_->playFlg != 0) {
            const auto step = static_cast<std::size_t>(
                std::min<std::int64_t>(kFramesPerRead, frame - framePos_));
            render(scratch_.data(), step);
            framePos_ += static_cast<std::int64_t>(step);
        }
        return framePos_;
    }

    void close() override {
        // The player holds a borrowed pointer to the song, so it goes first.
        player_.reset();
        song_.reset();
        scratch_.clear();
        framePos_ = 0;
    }

private:
    static constexpr std::int64_t kFramesPerRead = 2048;

    [[nodiscard]] std::int64_t fadeFrameCount() const {
        const double seconds =
            (settings_ != nullptr) ? settings_->SynthDefaultFadeSeconds() : 8.0;
        return static_cast<std::int64_t>(
            std::ceil(std::max(0.0, seconds) * static_cast<double>(rate_)));
    }

    /// How long the subsong runs, in frames, before any fade.
    ///
    /// jaytrax_getLength() plays the sequencer through without mixing and
    /// counts frames until either the module stops or it has looped as many
    /// times as asked. It gives up after thirty minutes and answers -1, and a
    /// module that reaches that has no length to report -- so it falls back on
    /// the same "how long is a synthesised track with no length" setting every
    /// other endless format here uses.
    [[nodiscard]] std::int64_t measureLength() {
        const int loops = (settings_ != nullptr) ? settings_->SynthDefaultLoopCount() : 2;
        // At least one pass: zero is a module of no length, which is not what
        // anybody means by "do not loop".
        const int passes = std::clamp(loops, 1, kMaxLoopCount);

        const std::int32_t frames = jaytrax_getLength(player_.get(), subsong_, passes,
                                                      static_cast<float>(rate_));
        // playFlg is read straight after, before anything else touches the
        // player: it is how the measurement says whether the module stopped on
        // its own or was still going when the loop count ran out.
        endsByItself_ = player_->playFlg == 0;

        if (frames > 0) {
            return frames;
        }
        const double seconds =
            (settings_ != nullptr) ? settings_->SynthDefaultSeconds() : 150.0;
        endsByItself_ = false;  // It did not end; the fade is what ends it.
        return static_cast<std::int64_t>(std::max(0.0, seconds) *
                                         static_cast<double>(rate_));
    }

    /// Renders `frames` frames of interleaved stereo int16.
    ///
    /// Zeroed first because the replayer's own no-channel path memsets half of
    /// what it should (jaytrax.c: `sizeof(int16_t) * nos` for `nos` stereo
    /// frames), leaving the rest of the buffer holding the previous chunk. A
    /// module with nrofchans == 0 is silent, and this is what makes it silent
    /// rather than a stutter.
    void render(std::int16_t* out, std::size_t frames) {
        std::fill_n(out, frames * kChannels, static_cast<std::int16_t>(0));
        jaytrax_renderChunk(player_.get(), out, static_cast<std::int32_t>(frames),
                            static_cast<float>(rate_));
    }

    /// Ramps the tail to silence, which is what turns a module that never ends
    /// into a track with a length. Not applied while looping for ever: that is
    /// the listener saying they did not want it to end.
    void applyFade(std::int16_t* frames, std::size_t count, bool endless) const {
        if (endless || fadeFrames_ <= 0 ||
            framePos_ + static_cast<std::int64_t>(count) <= bodyFrames_) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::int64_t position = framePos_ + static_cast<std::int64_t>(i);
            if (position <= bodyFrames_) {
                continue;
            }
            const double gain = std::max(0.0, static_cast<double>(totalFrames_ - position) /
                                                  static_cast<double>(fadeFrames_));
            for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
                std::int16_t& sample = frames[(i * kChannels) + channel];
                sample = static_cast<std::int16_t>(static_cast<double>(sample) * gain);
            }
        }
    }

    const Settings* settings_ = nullptr;

    SongPtr     song_;
    PlayerPtr   player_;
    AudioFormat format_{};

    int         rate_     = kDefaultRate;
    int         subsong_  = 0;
    int         subsongs_ = 0;
    std::string title_;

    bool         endsByItself_ = false;
    std::int64_t bodyFrames_   = 0;
    std::int64_t fadeFrames_   = 0;
    std::int64_t totalFrames_  = 0;
    std::int64_t framePos_     = 0;

    std::vector<std::int16_t> scratch_;
};

/// A module with more than one subsong expands to one URL per song, addressed
/// by fragment; a module with one stays a single row.
///
/// Cog expands unconditionally, so a one-subsong module -- which is most of
/// them -- becomes `something.jxs#0`. That is a fragment carrying no
/// information, and it makes the playlist entry differ from the file the
/// listener dragged in.
std::vector<Url> expandSubsongs(const Url& url, ISource& source,
                                const PluginRegistry& /*registry*/) {
    if (!url.fragment().empty()) {
        return {url};
    }

    const auto data = codecs::readAllBytes(source);
    if (!data) {
        return {url};
    }
    const SongPtr song = loadSong(*data);
    if (!song) {
        return {url};
    }

    const int count = subsongCount(*song);
    if (count <= 1) {
        return {url};
    }

    std::vector<Url> tracks;
    tracks.reserve(static_cast<std::size_t>(count));
    for (int subsong = 0; subsong < count; ++subsong) {
        tracks.push_back(url.withFragment(std::to_string(subsong)));
    }
    return tracks;
}

}  // namespace
}  // namespace xpcog

void xpcog_register_syntrax(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "SyntraxContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .expand     = &xpcog::expandSubsongs,
    });

    r.addDecoder({
        .name       = "SyntraxDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::SyntraxDecoder>();
        },
        .available = nullptr,
    });
}
