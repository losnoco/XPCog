// Game music rips, through Game_Music_Emu.
//
// Port of Cog Plugins/GME (GameDecoder, GameContainer, GameMetadataReader).
// These are not audio files: an NSF is 6502 code and a sound-chip program, and
// playing it means emulating the console. So there is no bitrate, no source
// format, and no natural length -- a chiptune loops for ever unless the rip says
// otherwise, which is what most of the awkwardness below is about.
//
// Three places this differs from Cog, each for a reason:
//
//   * It does not require a seekable source. Cog reads through gme_load_custom
//     with a size it gets by seeking to the end, so it refuses a stream
//     outright. Reading the file whole costs nothing here -- the emulator needs
//     all of it anyway -- and it means a rip plays over HTTP like anything else.
//
//   * The extension list and which formats are multi-track both come from the
//     library rather than from a hardcoded list. Cog hardcodes both, and its
//     lists include formats (sgc, sfm) that only exist in its patched build,
//     while omitting ones the stock library does support.
//
//   * Length comes from gme_info_t::play_length, which already *is* Cog's
//     fallback chain: the rip's own length, else intro + two loops, else 150
//     seconds. Those are exactly Cog's synthDefaultLoopCount (2) and
//     synthDefaultSeconds (150) defaults, so reimplementing the chain around
//     unreachable settings would land on the same numbers by a longer route.

#include "common/SourceBytes.hpp"
#include "common/TextEncoding.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <gme/gme.h>

// gme.h includes blargg_source.h, an internal header that does
// `#define byte byte_`. It is not meant to be part of the public interface --
// vcpkg installs it because the upstream build does -- and left standing it
// rewrites every later mention of std::byte into std::byte_. Undefining it here
// is narrower than fighting the include order, and the macro has no business
// surviving this file either way.
#undef byte

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// What Cog falls back to when synthSampleRate is unset or out of range, and
/// the clamp it applies.
constexpr int kDefaultRate = 44100;
constexpr int kMinRate     = 8000;
constexpr int kMaxRate     = 192000;

/// The SPC700 runs at 32 kHz and resampling it to 44.1 buys nothing but error,
/// so it overrides the preference rather than being clamped by it. Cog
/// special-cases the same type.
constexpr int kSpcSampleRate = 32000;

/// Cog's clamp on synthDefaultLoopCount.
constexpr int kMaxLoopCount = 10;

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// gme_type_extension() answers in upper case -- "SPC", "NSF" -- and the
/// registry matches on the lowercase extension a URL ends in. Without this the
/// decoder claims SPC, is never offered spc, and so is never offered anything.
void appendLowercase(std::vector<std::string>& list, const char* extension) {
    if (extension == nullptr || extension[0] == 0) {
        return;
    }
    std::string lowered{extension};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    list.push_back(std::move(lowered));
}

/// Every extension the linked library actually supports, asked of it rather
/// than listed. Cog hardcodes ay/gbs/hes/kss/nsf/nsfe/sap/sfm/sgc/spc, which
/// both over- and under-claims: sfm and sgc exist only in its patched build,
/// and it leaves out vgm, vgz and gym, which the stock library plays.
///
/// vgm and gym will eventually belong to a libvgmPlayer port, as they do in Cog.
/// Nothing has to change here when that happens -- registering it above this
/// decoder's priority is enough, and MultiDecoder falls back to this one.
[[nodiscard]] std::span<const std::string_view> supportedExtensions() {
    static const std::vector<std::string> owned = [] {
        std::vector<std::string> list;
        for (const gme_type_t* type = gme_type_list(); type != nullptr && *type != nullptr;
             ++type) {
            appendLowercase(list, gme_type_extension(*type));
        }
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

/// Only the formats that can hold more than one track. Asked of the library, so
/// a single-track format such as SPC is never expanded into `file.spc#0`.
[[nodiscard]] std::span<const std::string_view> multitrackExtensions() {
    static const std::vector<std::string> owned = [] {
        std::vector<std::string> list;
        for (const gme_type_t* type = gme_type_list(); type != nullptr && *type != nullptr;
             ++type) {
            if (gme_type_multitrack(*type) == 0) {
                continue;
            }
            appendLowercase(list, gme_type_extension(*type));
        }
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

using EmuPtr = std::unique_ptr<Music_Emu, decltype(&gme_delete)>;

[[nodiscard]] EmuPtr makeEmu(gme_type_t type, int sampleRate) {
    return EmuPtr{gme_new_emu(type, sampleRate), &gme_delete};
}

[[nodiscard]] int preferredRate(const Settings* settings) {
    if (settings == nullptr) {
        return kDefaultRate;
    }
    const int rate = settings->SynthSampleRate();
    return (rate < kMinRate || rate > kMaxRate) ? kDefaultRate : rate;
}

[[nodiscard]] int rateForType(gme_type_t type, const Settings* settings) {
    // Compared by name rather than against gme_spc_type. That constant is
    // exported *data*, and data symbols do not cross a DLL boundary on Windows
    // without __declspec(dllimport) -- which this header deliberately does not
    // emit for consumers, its BLARGG_EXPORT being left blank to stay "friendly
    // with both static and shared linking". Functions survive that on import
    // thunks; the type constants link-error.
    // Case-insensitively: gme_type_extension() answers "SPC", not "spc".
    const char* extension = gme_type_extension(type);
    if (extension == nullptr) {
        return preferredRate(settings);
    }
    std::string lowered{extension};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return (lowered == "spc") ? kSpcSampleRate : preferredRate(settings);
}

/// The subsong a fragment names, or 0 -- Cog's `[[url fragment] intValue]`,
/// which answers 0 for anything unparseable.
[[nodiscard]] int subsongFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    int                    value    = 0;
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + (c - '0');
    }
    return fragment.empty() ? 0 : value;
}

/// `game.nsf` -> `game.m3u`. Rips ship a sidecar playlist carrying the track
/// names and per-track lengths, which is the only place most of them exist:
/// an NSF itself knows how many tracks it has and nothing else about them.
[[nodiscard]] Url sidecarM3u(const Url& url) {
    std::string text = url.withoutFragment().toString();
    const auto  dot  = text.rfind('.');
    const auto  slash = text.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return {};
    }
    text.replace(dot, std::string::npos, ".m3u");
    return Url::parse(text).value_or(Url{});
}

/// Loads the sidecar into `emu`, if there is one. Silent when there is not:
/// most rips have none, and a missing sidecar is not a fault.
void loadSidecar(Music_Emu* emu, const Url& url, const PluginRegistry* registry) {
    if (registry == nullptr) {
        return;
    }
    const Url m3u = sidecarM3u(url);
    if (m3u.empty()) {
        return;
    }

    // Only the source, never registry->open(): opening it properly would hand
    // the file to the M3U *playlist* decoder, which is a different reading of
    // the same bytes.
    SourcePtr source = registry->makeSource(m3u);
    if (!source || !source->open(m3u)) {
        return;
    }

    const auto data = codecs::readAllBytes(*source);
    if (data && !data->empty()) {
        gme_load_m3u_data(emu, data->data(), static_cast<long>(data->size()));
    }
}

/// Cog's length chain, which gme_info_t::play_length also implements -- but with
/// the loop count and the default fixed at 2 and 150 s. Written out here because
/// both are now settings, and a rip that states a loop is exactly the case where
/// someone who wants three passes instead of two has to be able to say so.
[[nodiscard]] int playLengthMs(const gme_info_t& info, const Settings* settings) {
    if (info.length > 0) {
        return info.length;
    }

    if (info.loop_length > 0) {
        int loops = (settings != nullptr) ? settings->SynthDefaultLoopCount() : 2;
        loops     = std::clamp(loops, 0, kMaxLoopCount);
        const int intro = (info.intro_length > 0) ? info.intro_length : 0;
        return intro + loops * info.loop_length;
    }

    const double seconds =
        (settings != nullptr) ? settings->SynthDefaultSeconds() : 150.0;
    return static_cast<int>(std::ceil(std::max(0.0, seconds) * 1000.0));
}

/// Game_Music_Emu fades over eight seconds unless told otherwise, which is why
/// Cog's synthDefaultFadeSeconds is 8.0 -- the two were chosen to agree.
[[nodiscard]] int fadeLengthMs(const Settings* settings) {
    const double seconds =
        (settings != nullptr) ? settings->SynthDefaultFadeSeconds() : 8.0;
    return static_cast<int>(std::ceil(std::max(0.0, seconds) * 1000.0));
}

class GmeDecoder final : public IDecoder {
public:
    ~GmeDecoder() override { GmeDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr) {
            return false;
        }

        const Url&        url       = source->url();
        const std::string extension = url.extension();
        gme_type_t        type      = gme_identify_extension(extension.c_str());
        if (type == nullptr) {
            return false;
        }

        const auto data = codecs::readAllBytes(*source);
        if (!data || data->empty()) {
            return false;
        }

        rate_ = rateForType(type, settings_);
        emu_  = makeEmu(type, rate_);
        if (!emu_) {
            return false;
        }

        if (gme_load_data(emu_.get(), data->data(), static_cast<long>(data->size())) !=
            nullptr) {
            emu_.reset();
            return false;
        }

        loadSidecar(emu_.get(), url, registry_);

        track_ = subsongFromFragment(url);

        gme_info_t* info = nullptr;
        if (gme_track_info(emu_.get(), &info, track_) != nullptr || info == nullptr) {
            emu_.reset();
            return false;
        }
        readTags(*info, url);
        const int playMs = playLengthMs(*info, settings_);
        gme_free_info(info);

        if (gme_start_track(emu_.get(), track_) != nullptr) {
            emu_.reset();
            return false;
        }

        // Chiptunes do not end; they loop. The fade is what turns "for ever"
        // into a track with a length, so the reported duration has to include
        // the fade tail or playback would be cut off mid-fade.
        const int fadeMs = fadeLengthMs(settings_);
        gme_set_fade(emu_.get(), playMs);
        durationMs_ = playMs + fadeMs;

        format_.sampleRate    = static_cast<double>(rate_);
        format_.channels      = kChannels;
        format_.channelConfig = 0x3;  // FL | FR
        format_.format        = SampleFormat::S16;
        format_.bitsPerSample = 16;

        framePos_ = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = static_cast<std::int64_t>(
            static_cast<double>(durationMs_) * 0.001 * static_cast<double>(rate_));
        props.seekable = true;
        props.lossless = false;
        props.codec    = codec_;
        props.encoding = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

    bool readAudio(AudioChunk& out) override {
        if (!emu_ || gme_track_ended(emu_.get()) != 0) {
            return false;
        }

        if (gme_play(emu_.get(), static_cast<int>(kFramesPerRead * kChannels),
                     scratch_.data()) != nullptr) {
            return false;
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / static_cast<double>(rate_);
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(kFramesPerRead);
        std::memcpy(dst, scratch_.data(),
                    kFramesPerRead * kChannels * sizeof(std::int16_t));
        out.setFrameCount(kFramesPerRead);

        framePos_ += static_cast<std::int64_t>(kFramesPerRead);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (!emu_) {
            return -1;
        }
        const auto ms = static_cast<int>(static_cast<double>(frame) * 1000.0 /
                                         static_cast<double>(rate_));
        if (gme_seek(emu_.get(), ms) != nullptr) {
            return -1;
        }
        framePos_ = frame;
        return frame;
    }

    void close() override { emu_.reset(); }

private:
    void readTags(const gme_info_t& info, const Url& url) {
        const auto put = [this](std::string_view key, const char* value) {
            if (value != nullptr && *value != '\0') {
                tags_.set(key, codecs::toUtf8(value));
            }
        };

        // Cog's mapping, including putting the console in `genre` -- odd, and
        // kept, because it is what a Cog user's library already holds.
        put("genre", info.system);
        put("album", info.game);
        put("artist", info.author);
        put("copyright", info.copyright);
        put("comment", info.comment);

        if (info.song != nullptr && *info.song != '\0') {
            tags_.set("title", codecs::toUtf8(info.song));
        } else {
            // Without this every track of an NSF shows as the same filename,
            // which is Cog's reason for the same fallback.
            std::string name{url.extension().empty() ? url.toString()
                                                     : url.withoutFragment().toString()};
            if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) {
                name.erase(0, slash + 1);
            }
            tags_.set("title", name + " [" + std::to_string(track_) + "]");
        }

        tags_.set("track", std::to_string(track_ + 1));

        if (info.system != nullptr && *info.system != '\0') {
            codec_ = codecs::toUtf8(info.system);
        }
    }

    const PluginRegistry* registry_ = nullptr;
    const Settings*       settings_ = nullptr;

    EmuPtr       emu_{nullptr, &gme_delete};
    AudioFormat  format_{};
    int          rate_       = kDefaultRate;
    int          track_      = 0;
    int          durationMs_ = 0;
    std::int64_t framePos_   = 0;
    std::string  codec_;
    MetadataMap  tags_;

    std::array<std::int16_t, kFramesPerRead * kChannels> scratch_{};
};

/// A multi-track rip expands to one URL per track, addressed by fragment. The
/// sidecar is loaded first so the track *count* reflects it: an m3u can name
/// fewer or more entries than the rip's own header claims, and the sidecar is
/// the more accurate of the two.
std::vector<Url> expandRip(const Url& url, ISource& source,
                           const PluginRegistry& /*registry*/) {
    if (!url.fragment().empty()) {
        return {url};
    }

    const std::string extension = url.extension();
    gme_type_t        type      = gme_identify_extension(extension.c_str());
    if (type == nullptr) {
        return {url};
    }

    const auto data = codecs::readAllBytes(source);
    if (!data || data->empty()) {
        return {url};
    }

    // gme_info_only: no synthesis wanted, only the track count.
    EmuPtr emu = makeEmu(type, gme_info_only);
    if (!emu ||
        gme_load_data(emu.get(), data->data(), static_cast<long>(data->size())) !=
            nullptr) {
        return {url};
    }

    const int count = gme_track_count(emu.get());
    if (count <= 1) {
        return {url};
    }

    std::vector<Url> tracks;
    tracks.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        tracks.push_back(url.withFragment(std::to_string(i)));
    }
    return tracks;
}

}  // namespace
}  // namespace xpcog

void xpcog_register_gme(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "GmeContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::multitrackExtensions(),
        .mimeTypes  = {},
        .expand     = &xpcog::expandRip,
    });

    r.addDecoder({
        .name       = "GmeDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::supportedExtensions(),
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::GmeDecoder>();
        },
        .available = nullptr,
    });
}
