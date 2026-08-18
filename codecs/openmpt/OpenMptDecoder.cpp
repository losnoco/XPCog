// Tracker modules, through libopenmpt.
//
// Port of Cog Plugins/OpenMPT (OMPTDecoder, OMPTContainer, OMPTMetadataReader).
// One decoder covering every format libopenmpt knows -- MOD, XM, IT, S3M, MPTM
// and a few dozen more -- which is the first real test of the claim that adding
// a decoder here is one xpcog_add_codec() call rather than a refactor.
//
// Modules are not streamed. libopenmpt parses the whole file and then
// synthesises from it, so this reads the source whole at open() and never
// touches it again. That is also why the format is fixed float stereo: there is
// no source format to honour, only a rate to render at.
//
// The extension list is the interesting difference from every codec before it.
// Every other one here knows its extensions at compile time; libopenmpt answers
// at runtime, and the answer grows when the library is updated. The registry
// wants a span that outlives it, so the list is built once into a function-local
// static -- which is exactly as long-lived as the registry itself.

#include "common/SourceBytes.hpp"
#include "common/TextEncoding.hpp"

#ifdef XPCOG_HAVE_ARCHIVE
// Declarations only -- no libarchive header comes with it, and the guard means
// this is never included unless that codec is being built alongside.
#    include "archive/CompressedFileSource.hpp"
#endif

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <libopenmpt/libopenmpt.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// What Cog falls back to when synthSampleRate is unset or out of range, and
/// the clamp it applies.
constexpr std::int32_t kDefaultRate = 44100;
constexpr std::int32_t kMinRate     = 8000;
constexpr std::int32_t kMaxRate     = 192000;

[[nodiscard]] std::int32_t renderRate(const Settings* settings) {
    if (settings == nullptr) {
        return kDefaultRate;
    }
    const auto rate = static_cast<std::int32_t>(settings->SynthSampleRate());
    return (rate < kMinRate || rate > kMaxRate) ? kDefaultRate : rate;
}

/// libopenmpt's interpolation filter length, from the shared `resampling`
/// quality tier.
///
/// Cog maps this preference by *algorithm* name -- zoh and blep to 1, linear and
/// blam to 2, cubic to 4, sinc to 8 -- because those are the names its own
/// resampler uses. XPCog's resampler is soxr, whose vocabulary is quality tiers,
/// and the key is shared rather than duplicated (see settings.def). So the tier
/// picks the filter, which lands on the same filter Cog would for anyone who
/// never changed the setting: Cog defaults to sinc, XPCog to high, and both mean
/// 8 taps here.
[[nodiscard]] std::int32_t interpolationFilter(const Settings* settings) {
    if (settings == nullptr) {
        return 8;
    }
    const std::string quality = settings->Resampling();
    if (quality == "quick") {
        return 1;  // nearest-neighbour, as zoh is for Cog
    }
    if (quality == "low") {
        return 2;  // linear
    }
    if (quality == "medium") {
        return 4;  // cubic
    }
    return 8;  // high and best, and anything unrecognised: sinc
}

constexpr std::uint32_t kChannels       = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// Every extension this build can open a module from, lowercase, built once.
///
/// libopenmpt's own list, plus -- when the archive codec is in the build --
/// mdz, mdr, s3z, xmz, itz and mptmz, which are a single module inside a zip or
/// a RAR. libopenmpt does not unpack those; the archive codec registers a source
/// wrapper that hands this decoder the module inside, and the list comes from
/// there rather than being restated here, so the two can never disagree about
/// which of them is claimed.
///
/// The guard matters: advertising a format nothing can then open is worse than
/// not advertising it, since it fills the open dialog with files that fail.
[[nodiscard]] std::span<const std::string_view> supportedExtensions() {
    static const std::vector<std::string> owned = [] {
        std::vector<std::string> list;
        try {
            list = openmpt::get_supported_extensions();
        } catch (const std::exception&) {
            // A library that cannot enumerate its own formats claims none, and
            // the registry simply never offers this decoder.
            list.clear();
        }
#ifdef XPCOG_HAVE_ARCHIVE
        for (const std::string_view extension : codecs::compressedModuleExtensions()) {
            list.emplace_back(extension);
        }
#endif
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
        return list;
    }();

    static const std::vector<std::string_view> views = [] {
        std::vector<std::string_view> out;
        out.reserve(owned.size());
        for (const std::string& extension : owned) {
            out.emplace_back(extension);
        }
        return out;
    }();

    return views;
}

constexpr std::string_view kMimeTypes[] = {"audio/x-it", "audio/x-xm",
                                           "audio/x-s3m", "audio/x-mod"};

/// Builds a module from a source, or nullptr. Shared by the decoder and the
/// container, both of which do the same thing: read the file whole and hand it
/// to libopenmpt.
[[nodiscard]] std::unique_ptr<openmpt::module> loadModule(ISource& source) {
    const auto data = codecs::readAllBytes(source);
    if (!data || data->empty()) {
        return nullptr;
    }

    try {
        std::map<std::string, std::string> ctls;
        // Without this a seek lands on a pattern row and reconstructs channel
        // state from there, which is fast and audibly wrong -- notes that should
        // still be ringing are silent. Cog sets it for the same reason.
        ctls["seek.sync_samples"] = "1";
        return std::make_unique<openmpt::module>(data->data(), data->size(),
                                                 std::clog, ctls);
    } catch (const std::exception&) {
        // Not a module, or a corrupt one. libopenmpt reports both by throwing,
        // and the registry's answer to either is the next candidate decoder.
        return nullptr;
    }
}

/// The subsong a fragment names, or 0. Cog's `[[url fragment] intValue]`, which
/// answers 0 for anything unparseable.
[[nodiscard]] std::int32_t subsongFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    if (fragment.empty()) {
        return 0;
    }
    std::int32_t value = 0;
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

class OpenMptDecoder final : public IDecoder {
public:
    ~OpenMptDecoder() override { OpenMptDecoder::close(); }

    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr) {
            return false;
        }

        module_ = loadModule(*source);
        if (!module_) {
            return false;
        }

        renderRate_ = renderRate(settings_);

        try {
            module_->select_subsong(subsongFromFragment(source->url()));

            // Cog passes -1 here under repeat-one, which makes the module loop
            // inside libopenmpt for ever. A decoder here cannot see the
            // playlist's repeat mode, and an endless track is a poor thing to
            // default to: it reports no duration and never advances. Repeat is
            // the playlist's job in XPCog, and it can repeat this like anything
            // else.
            module_->set_repeat_count(0);

            module_->set_render_param(
                openmpt::module::RENDER_MASTERGAIN_MILLIBEL, 0);
            module_->set_render_param(
                openmpt::module::RENDER_STEREOSEPARATION_PERCENT, 100);
            module_->set_render_param(
                openmpt::module::RENDER_INTERPOLATIONFILTER_LENGTH,
                interpolationFilter(settings_));
            module_->set_render_param(
                openmpt::module::RENDER_VOLUMERAMPING_STRENGTH, -1);
            // Amiga modules were written against that hardware's filter, and
            // without emulating it they are brighter than they were meant to be.
            module_->ctl_set_boolean("render.resampler.emulate_amiga", true);

            duration_ = module_->get_duration_seconds();
            readTags();
        } catch (const std::exception&) {
            module_.reset();
            return false;
        }

        format_.sampleRate    = static_cast<double>(renderRate_);
        format_.channels      = kChannels;
        format_.channelConfig = 0x3;  // FL | FR
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;

        framePos_ = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames =
            static_cast<std::int64_t>(duration_ * static_cast<double>(renderRate_));
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = codec_;
        // Cog's own word for a decoder that generates rather than decompresses.
        props.encoding = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

    bool readAudio(AudioChunk& out) override {
        if (!module_) {
            return false;
        }

        std::size_t rendered = 0;
        try {
            rendered = module_->read_interleaved_stereo(
                renderRate_, kFramesPerRead, scratch_.data());
        } catch (const std::exception&) {
            return false;
        }

        if (rendered == 0) {
            return false;  // end of the module
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp =
            static_cast<double>(framePos_) / static_cast<double>(renderRate_);
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(rendered);
        std::memcpy(dst, scratch_.data(), rendered * kChannels * sizeof(float));
        out.setFrameCount(rendered);

        framePos_ += static_cast<std::int64_t>(rendered);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (!module_) {
            return -1;
        }
        try {
            module_->set_position_seconds(static_cast<double>(frame) /
                                          static_cast<double>(renderRate_));
        } catch (const std::exception&) {
            return -1;
        }
        framePos_ = frame;
        return frame;
    }

    void close() override { module_.reset(); }

private:
    void readTags() {
        // Cog drops "type" and renames "type_long" to codec; everything else it
        // keeps under libopenmpt's own key. The keys are ASCII, but the values
        // are whatever the tracker wrote in 1994, so they go through the same
        // UTF-8-or-Latin-1 rule as every other untagged text here.
        for (const std::string& key : module_->get_metadata_keys()) {
            const std::string value = module_->get_metadata(key);
            if (value.empty() || key == "type") {
                continue;
            }
            if (key == "type_long") {
                codec_ = codecs::toUtf8(value);
                continue;
            }
            tags_.set(key, codecs::toUtf8(value));
        }
    }

    const Settings*                  settings_   = nullptr;
    std::int32_t                     renderRate_ = kDefaultRate;
    std::unique_ptr<openmpt::module> module_;
    AudioFormat                      format_{};
    double                           duration_ = 0.0;
    std::int64_t                     framePos_ = 0;
    std::string                      codec_;
    MetadataMap                      tags_;

    std::array<float, kFramesPerRead * kChannels> scratch_{};
};

/// A module with several subsongs expands to one URL per subsong, addressed by
/// fragment -- the same shape a cue sheet uses. A module with one subsong stays
/// a single entry rather than becoming `file.it#0`, so the common case does not
/// grow a fragment nobody asked for.
std::vector<Url> expandModule(const Url& url, ISource& source,
                              const PluginRegistry& /*registry*/) {
    if (!url.fragment().empty()) {
        return {url};  // already addressed; expanding again would recurse
    }

    const std::unique_ptr<openmpt::module> module = loadModule(source);
    if (!module) {
        return {url};
    }

    std::int32_t subsongs = 0;
    try {
        subsongs = module->get_num_subsongs();
    } catch (const std::exception&) {
        return {url};
    }

    if (subsongs <= 1) {
        return {url};
    }

    std::vector<Url> tracks;
    tracks.reserve(static_cast<std::size_t>(subsongs));
    for (std::int32_t i = 0; i < subsongs; ++i) {
        tracks.push_back(url.withFragment(std::to_string(i)));
    }
    return tracks;
}

}  // namespace
}  // namespace xpcog

void xpcog_register_openmpt(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "OpenMptContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::supportedExtensions(),
        .mimeTypes  = {},
        .expand     = &xpcog::expandModule,
    });

    r.addDecoder({
        .name       = "OpenMptDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::supportedExtensions(),
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::OpenMptDecoder>();
        },
        .available = nullptr,
    });
}
