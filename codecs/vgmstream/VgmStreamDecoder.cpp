// Streamed audio from video games, through vgmstream.
//
// Port of Cog Plugins/vgmstream. Several hundred container formats -- BRSTM,
// BCSTM, BFSTM, BWAV, HPS, AST, ADX, AAX, HCA and a long tail -- which between
// them are what a console rip actually is. Most are a header wrapped around
// ADPCM or PCM, which is why the base build covers them without any optional
// codec library.
//
// Three things make this decoder different from the ones before it.
//
// **Priority is below default.** vgmstream claims `wav`, `ogg`, `mp3` and other
// extensions it can technically read, because a game archive may hold any of
// them. Left at the default it would win ties against FLAC, FFmpeg and mpg123
// for ordinary music files. Registered lower, the dedicated decoder is tried
// first and vgmstream is what catches the file it declined -- which is the
// MultiDecoder fallback doing exactly what it was written for.
//
// **Companion files.** vgmstream reads more than the file it was handed: an
// `.aw` bank needs its `.bms`, dual-mono `.dsp` rips come in pairs, and a
// `.txtp` is a text file naming the streams it wants. Its streamfile interface
// has an `open` callback for that, and it is routed back through the registry
// rather than to stdio -- so a companion file resolves the same way inside an
// archive or over HTTP as it does on disk. Cog reaches for its own file layer
// here; passing the registry keeps the seam.
//
// **Loops are declined.** Game music loops, often forever, and vgmstream will
// happily render an endless stream. A track that never ends reports no duration
// and never advances the playlist, which is the same reason the module decoder
// passes 0 for its repeat count. The playlist repeats things here.

#include "common/SourceBytes.hpp"
#include "common/TextEncoding.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

// Under the install prefix vcpkg gives it. libvgmstream.h pulls in the
// streamfile header itself, with a quoted include that resolves beside it.
//
// The extern "C" is ours, not upstream's: vgmstream is a C library exporting C
// symbols, but its public headers carry no __cplusplus guard at all, so a C++
// translation unit declares every entry point with C++ linkage and the names
// mangle. It compiles cleanly and fails at link with five undefined symbols
// that look exactly like a missing library.
extern "C" {
#include <vgmstream/libvgmstream.h>
}

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {



/// Below kDefaultPriority, so a dedicated decoder wins any extension both claim.
constexpr Priority kVgmStreamPriority = 0.5f;

// --- the streamfile bridge ----------------------------------------------

/// What vgmstream reads through. Owns the source when it opened one itself (a
/// companion file), and borrows it when the decoder handed over the track's own.
struct SourceStreamFile {
    libstreamfile_t  libsf{};   ///< must stay first: vgmstream frees this pointer
    ISource*         source = nullptr;
    SourcePtr        owned;     ///< set only for companion files
    const PluginRegistry* registry = nullptr;
    std::string      name;
    std::int64_t     size = -1;
};

libstreamfile_t* makeStreamFile(SourcePtr owned, ISource* borrowed,
                                const PluginRegistry* registry);

int sfRead(void* user, std::uint8_t* dst, std::int64_t offset, int length) {
    auto* self = static_cast<SourceStreamFile*>(user);
    if (self->source == nullptr || dst == nullptr || length <= 0) {
        return 0;
    }
    // vgmstream reads positionally and assumes the seek always lands; a source
    // that refuses is at end of file, which is a read of zero rather than an
    // error.
    if (!self->source->seek(offset, SEEK_SET)) {
        return 0;
    }
    const std::int64_t got = self->source->read(dst, length);
    return got < 0 ? 0 : static_cast<int>(got);
}

std::int64_t sfGetSize(void* user) {
    auto* self = static_cast<SourceStreamFile*>(user);
    if (self->size >= 0) {
        return self->size;
    }
    if (self->source == nullptr || !self->source->seekable()) {
        return 0;
    }
    const std::int64_t here = self->source->tell();
    if (!self->source->seek(0, SEEK_END)) {
        return 0;
    }
    self->size = self->source->tell();
    self->source->seek(here, SEEK_SET);
    return self->size;
}

const char* sfGetName(void* user) {
    return static_cast<SourceStreamFile*>(user)->name.c_str();
}

/// vgmstream asking for a companion file, by absolute path in its own idea of
/// the filesystem. Resolved through the registry, so the answer is a real
/// ISource and the mechanism works wherever the track itself came from.
libstreamfile_t* sfOpen(void* user, const char* filename) {
    auto* self = static_cast<SourceStreamFile*>(user);
    if (self->registry == nullptr || filename == nullptr) {
        return nullptr;
    }

    const Url url = Url::fromLocalPath(pathFromUtf8(filename));
    SourcePtr source = self->registry->makeSource(url);
    if (!source || !source->open(url)) {
        return nullptr;  // vgmstream treats null as "not there", which it is
    }
    return makeStreamFile(std::move(source), nullptr, self->registry);
}

void sfClose(libstreamfile_t* libsf) {
    delete reinterpret_cast<SourceStreamFile*>(libsf);
}

libstreamfile_t* makeStreamFile(SourcePtr owned, ISource* borrowed,
                                const PluginRegistry* registry) {
    auto* self     = new SourceStreamFile;
    self->owned    = std::move(owned);
    self->source   = self->owned ? self->owned.get() : borrowed;
    self->registry = registry;
    if (self->source != nullptr) {
        // vgmstream picks meta parsers by extension, so the name it sees has to
        // carry one. A local path when there is one, the URL otherwise.
        const auto path = self->source->url().localPath();
        self->name      = path ? pathToUtf8(*path) : self->source->url().toString();
    }

    self->libsf.user_data = self;
    self->libsf.read      = &sfRead;
    self->libsf.get_size  = &sfGetSize;
    self->libsf.get_name  = &sfGetName;
    self->libsf.open      = &sfOpen;
    self->libsf.close     = &sfClose;
    return &self->libsf;
}

// --- extensions ---------------------------------------------------------

/// Every extension this build of vgmstream claims, lowercase, built once.
/// Answered at run time like libopenmpt's, and for the same reason: the list
/// grows when the library is updated and a hard-coded copy would drift.
[[nodiscard]] std::span<const std::string_view> supportedExtensions() {
    static const std::vector<std::string> owned = [] {
        std::vector<std::string> list;
        int          count = 0;
        const char** names = libvgmstream_get_extensions(&count);
        if (names != nullptr) {
            for (int i = 0; i < count; ++i) {
                if (names[i] != nullptr && names[i][0] != '\0') {
                    list.emplace_back(names[i]);
                }
            }
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

/// The subsong a fragment names, or 0. vgmstream counts subsongs from 1, and 0
/// means "whatever the file opens as by default".
[[nodiscard]] int subsongFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    int                    value    = 0;
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

class VgmStreamDecoder final : public IDecoder {
public:
    ~VgmStreamDecoder() override { VgmStreamDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr) {
            return false;
        }

        libstreamfile_t* libsf = makeStreamFile(nullptr, source, registry_);
        if (libsf == nullptr) {
            return false;
        }

        libvgmstream_config_t config{};
        // One loop and no fade, then stop. See the note at the top: an endless
        // track reports no duration and never advances the playlist.
        config.loop_count  = 1.0;
        config.ignore_fade = true;
        // Everything is rendered as float, so there is one conversion path
        // rather than four, and the engine's chain is float already.
        config.force_sfmt = LIBVGMSTREAM_SFMT_FLOAT;

        lib_ = libvgmstream_create(libsf, subsongFromFragment(source->url()), &config);
        // libvgmstream_create consumes the streamfile whether or not it
        // succeeded, so there is nothing to free here on failure.
        if (lib_ == nullptr) {
            return false;
        }

        const libvgmstream_format_t* format = lib_->format;
        if (format == nullptr || format->channels <= 0 || format->sample_rate <= 0) {
            close();
            return false;
        }

        format_.sampleRate    = static_cast<double>(format->sample_rate);
        format_.channels      = static_cast<std::uint32_t>(format->channels);
        format_.channelConfig = format->channel_layout;
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;

        totalFrames_ = format->play_samples;
        bitrateKbps_ = format->stream_bitrate / 1000;
        codec_       = codecs::toUtf8(format->codec_name);
        container_   = codecs::toUtf8(format->meta_name);
        streamName_  = codecs::toUtf8(format->stream_name);

        framePos_ = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.bitrateKbps = bitrateKbps_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = codec_;
        // Cog's word for a decoder that is neither a plain file format nor a
        // synthesiser: the audio is real samples, wrapped in a game's container.
        props.encoding = "lossy";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override {
        MetadataMap tags;
        // The stream's own name, which for a bank of sound effects is the only
        // thing telling one subsong from another.
        if (!streamName_.empty()) {
            tags.set("title", streamName_);
        }
        if (!container_.empty()) {
            tags.set("container", container_);
        }
        return tags;
    }

    bool readAudio(AudioChunk& out) override {
        if (lib_ == nullptr) {
            return false;
        }

        // Through libvgmstream_render(), which hands back its own buffer, rather
        // than libvgmstream_fill(), which copies into ours. Not a performance
        // choice: on r2117 fill() returns 0 samples forever and never sets the
        // done flag, while render() on the same file and the same handle
        // produces 1024 samples on the first call. Measured against vgmstream's
        // own stdio reader, so it is the library's path that is broken and not
        // this bridge -- which is what the first version of this decoder tripped
        // over, silently decoding every game rip to zero frames.
        const int rc = libvgmstream_render(lib_);
        if (rc < 0 || lib_->decoder == nullptr) {
            return false;  // a decode error, which vgmstream has logged
        }

        const int frames = lib_->decoder->buf_samples;
        if (frames <= 0) {
            return false;  // end of stream; done is set on the last real buffer
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / format_.sampleRate;
        out.streamTimeRatio = 1.0;

        const auto count = static_cast<std::size_t>(frames);
        std::byte* dst   = out.allocFrames(count);
        std::memcpy(dst, lib_->decoder->buf,
                    static_cast<std::size_t>(lib_->decoder->buf_bytes));
        out.setFrameCount(count);

        framePos_ += frames;
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (lib_ == nullptr) {
            return -1;
        }
        libvgmstream_seek(lib_, frame);
        framePos_ = frame;
        return frame;
    }

    void close() override {
        if (lib_ != nullptr) {
            libvgmstream_free(lib_);
            lib_ = nullptr;
        }
    }

private:
    const PluginRegistry* registry_ = nullptr;
    libvgmstream_t*       lib_      = nullptr;

    AudioFormat        format_{};
    std::int64_t       totalFrames_ = 0;
    std::int32_t       bitrateKbps_ = 0;
    std::int64_t       framePos_    = 0;
    std::string        codec_;
    std::string        container_;
    std::string        streamName_;
};

/// A bank with several subsongs expands to one URL per subsong, addressed by
/// fragment -- the shape a cue sheet and a multi-song module already use. A file
/// with one stays a single entry rather than growing a `#1` nobody asked for.
std::vector<Url> expandVgmStream(const Url& url, ISource& source,
                                 const PluginRegistry& registry) {
    if (!url.fragment().empty()) {
        return {url};  // already addressed; expanding again would recurse
    }

    libstreamfile_t* libsf = makeStreamFile(nullptr, &source, &registry);
    if (libsf == nullptr) {
        return {url};
    }

    libvgmstream_config_t config{};
    config.loop_count = 1.0;

    libvgmstream_t* lib = libvgmstream_create(libsf, 0, &config);
    if (lib == nullptr) {
        return {url};
    }

    const int subsongs = (lib->format != nullptr) ? lib->format->subsong_count : 0;
    libvgmstream_free(lib);

    if (subsongs <= 1) {
        return {url};
    }

    std::vector<Url> tracks;
    tracks.reserve(static_cast<std::size_t>(subsongs));
    for (int i = 1; i <= subsongs; ++i) {
        tracks.push_back(url.withFragment(std::to_string(i)));
    }
    return tracks;
}

}  // namespace
}  // namespace xpcog

void xpcog_register_vgmstream(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "VgmStreamContainer",
        .priority   = xpcog::kVgmStreamPriority,
        .extensions = xpcog::supportedExtensions(),
        .mimeTypes  = {},
        .expand     = &xpcog::expandVgmStream,
    });

    r.addDecoder({
        .name       = "VgmStreamDecoder",
        .priority   = xpcog::kVgmStreamPriority,
        .extensions = xpcog::supportedExtensions(),
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::VgmStreamDecoder>();
        },
        .available = nullptr,
    });
}
