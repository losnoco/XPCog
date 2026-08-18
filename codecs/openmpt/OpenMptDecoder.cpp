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

#include "xpcog/core/Plugin.hpp"
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

/// Cog reads `synthSampleRate` and clamps it to 8000..192000. A codec here
/// cannot reach Settings -- core takes them by injection and a decoder is
/// constructed with no arguments -- so this is Cog's fallback, which is also
/// what Cog uses whenever that default is unset or out of range.
constexpr std::int32_t kRenderRate = 44100;
constexpr double       kSampleRate = static_cast<double>(kRenderRate);

/// Cog maps its `resampling` preference onto this; 8 is the sinc filter, which
/// is what that preference defaults to. Same settings-reachability reason.
constexpr std::int32_t kInterpolationFilter = 8;

constexpr std::uint32_t kChannels = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// Every extension libopenmpt claims, lowercase, built once.
///
/// Deliberately *not* including Cog's archive extensions -- mdz, mdr, s3z, xmz,
/// itz, mptmz. Those are zipped modules, which libopenmpt does not unpack; Cog
/// can claim them because its ArchiveSource unpacks them first. Claiming them
/// here would advertise formats that cannot be opened, which is worse than not
/// advertising them. They belong with ArchiveSource when it lands.
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
        std::sort(list.begin(), list.end());
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

/// Builds a module from a source, or nullptr. Shared by the decoder, the
/// container and the metadata reader, all three of which do the same thing:
/// read the file whole and hand it to libopenmpt.
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
    const auto*  begin = fragment.data();
    const auto*  end   = begin + fragment.size();
    for (const char* at = begin; at != end; ++at) {
        if (*at < '0' || *at > '9') {
            return 0;
        }
        value = value * 10 + (*at - '0');
    }
    return value;
}

class OpenMptDecoder final : public IDecoder {
public:
    ~OpenMptDecoder() override { OpenMptDecoder::close(); }

    bool open(ISource* source) override {
        close();
        if (source == nullptr) {
            return false;
        }

        module_ = loadModule(*source);
        if (!module_) {
            return false;
        }

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
                kInterpolationFilter);
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

        format_.sampleRate    = kSampleRate;
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
        props.totalFrames = static_cast<std::int64_t>(duration_ * kSampleRate);
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
                kRenderRate, kFramesPerRead, scratch_.data());
        } catch (const std::exception&) {
            return false;
        }

        if (rendered == 0) {
            return false;  // end of the module
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / kSampleRate;
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
            module_->set_position_seconds(static_cast<double>(frame) / kSampleRate);
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
std::vector<Url> expandModule(const Url& url, ISource& source) {
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
