// AdLib and OPL2 music, through AdPlug.
//
// Port of Cog Plugins/AdPlug (AdPlugDecoder, AdPlugContainer,
// AdPlugMetadataReader, fileprovider). Around forty formats from the DOS era --
// AdLib Tracker, Reality ADlib Tracker, HSC, Creative's CMF, Loudness, EdLib,
// Sierra's and Westwood's in-house formats among them -- none of which are
// audio files. Each is a sequence plus a set of OPL2 register programs, and
// playing one means emulating the chip.
//
// AdPlug's shape is unlike every other decoder here and drives most of what
// follows. There is no open-this-buffer entry point: CAdPlug::factory() is
// handed a *filename* and a CFileProvider, and walks its whole player list
// asking each in turn to load it. So this codec has to give it a file
// abstraction rather than bytes -- see SourceProvider below, which is the
// interesting half of the file.
//
// Four things differ from Cog, each for a reason:
//
//   * **The extension list comes from the library and is materialised once.**
//     Cog asks CAdPlug::players for it on every call to +fileTypes.
//     DecoderDescriptor::extensions is a span frozen at registration, so the
//     list is built into function-local storage the first time it is asked for
//     and the span points at that. Nothing is hardcoded either way: a player
//     added upstream appears without an edit here.
//
//   * **The database is compiled in** rather than loaded from a bundle
//     resource. See AdPlugDatabase.cpp.
//
//   * **A non-seekable source is refused, and that is not laziness.** Five of
//     AdPlug's players open a *second* file beside the first -- KSM wants
//     `insts.dat`, ROL a `.bnk` bank, and the factory rewinds and re-reads the
//     primary for every player it tries. Neither survives a stream.
//
//   * **Priority, and the one collision that matters.** Cog runs AdPlug at 0.5
//     against a default of 1.0 and a vgmstream of 0.0 -- above the catch-alls,
//     below everything dedicated. That ordering is what makes `.raw` come out
//     right: an AdLib Rdos OPL capture and a headerless PCM dump share the
//     extension, and vgmstream accepts either. This tree's catch-alls sit at
//     0.5 rather than 0.0, so the same *relative* order is 0.75 here. Getting
//     this wrong is not a crash: `.raw` plays, as noise.

#include "AdPlugDatabase.hpp"

#include "common/TextEncoding.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

#include <adplug/adplug.h>
#include <adplug/nemuopl.h>
#include <adplug/silentopl.h>

#include <binio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

/// Above this tree's catch-alls (FFmpeg and vgmstream, both 0.5) and below every
/// dedicated decoder. See the file comment for why that ordering is the whole
/// point rather than a detail.
constexpr Priority kAdPlugPriority = 0.75F;

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// What Cog falls back to when synthSampleRate is unset or out of range, and
/// the clamp it applies.
constexpr int kDefaultRate = 44100;
constexpr int kMinRate     = 8000;
constexpr int kMaxRate     = 192000;

[[nodiscard]] int preferredRate(const Settings* settings) {
    if (settings == nullptr) {
        return kDefaultRate;
    }
    const int rate = settings->SynthSampleRate();
    return (rate < kMinRate || rate > kMaxRate) ? kDefaultRate : rate;
}

// ---------------------------------------------------------------------------
// Giving AdPlug a file
// ---------------------------------------------------------------------------

/// A binistream over an ISource, with a small read-ahead buffer.
///
/// libbinio's reading interface is one virtual call per byte, and AdPlug's
/// loaders use it that way -- so without a buffer here every byte of a module
/// would be a call through ISource into the filesystem. Cog buffers for the
/// same reason and at the same size.
class SourceStream final : public binistream {
public:
    /// `owned` is non-null only for a companion file this stream opened itself;
    /// the primary source belongs to the decoder and outlives every stream over
    /// it.
    SourceStream(ISource* source, SourcePtr owned)
        : source_(source), owned_(std::move(owned)) {}

    void seek(long position, Offset whence) override {
        std::int64_t target = position;
        switch (whence) {
            case Set:
                break;
            case Add:
                // Relative to where the *reader* is, which is behind the
                // source by whatever is still unread in the buffer. Getting
                // this wrong reads as a corrupt file rather than as a seek bug.
                target = pos() + position;
                break;
            case End:
                if (!source_->seek(0, SEEK_END)) {
                    err |= Fatal;
                    return;
                }
                target = source_->tell() + position;
                break;
        }

        if (target < 0 || !source_->seek(target, SEEK_SET)) {
            err |= Fatal;
            return;
        }
        err &= ~Eof;
        filled_ = 0;
        at_     = 0;
    }

    long pos() override {
        return static_cast<long>(source_->tell() - (filled_ - at_));
    }

protected:
    Byte getByte() override {
        if ((err & Eof) != 0) {
            return 0;
        }
        if (at_ == filled_) {
            const std::int64_t got = source_->read(buffer_.data(),
                                                   static_cast<std::int64_t>(buffer_.size()));
            if (got <= 0) {
                err |= Eof;
                return 0;
            }
            filled_ = static_cast<std::size_t>(got);
            at_     = 0;
        }
        return static_cast<Byte>(buffer_[at_++]);
    }

private:
    ISource*  source_;
    SourcePtr owned_;

    std::vector<unsigned char> buffer_ = std::vector<unsigned char>(4096);
    std::size_t                filled_ = 0;
    std::size_t                at_     = 0;
};

/// What AdPlug opens files through.
///
/// The factory calls open() once per player it tries -- forty-odd times for a
/// file nothing claims -- and five players call it again for a companion file
/// beside the first: KSM's `insts.dat`, ROL's instrument bank, and the ones in
/// adtrack, mid and mus. Those companions are derived by string surgery on the
/// name the factory was given, so what is handed in has to be something that
/// survives having its last path component replaced. A percent-decoded URL does;
/// a raw local path would too, until the first module reached over HTTP.
class SourceProvider final : public CFileProvider {
public:
    SourceProvider(std::string primaryPath, ISource* primary,
                   const PluginRegistry* registry)
        : primaryPath_(std::move(primaryPath)), primary_(primary), registry_(registry) {}

    binistream* open(std::string filename) const override {
        if (filename == primaryPath_ && primary_ != nullptr) {
            // Rewound, because the factory hands the same file to one player
            // after another and each expects to start at the beginning.
            if (!primary_->seek(0, SEEK_SET)) {
                return nullptr;
            }
            return finish(std::make_unique<SourceStream>(primary_, nullptr));
        }

        if (registry_ == nullptr) {
            return nullptr;
        }

        const std::optional<Url> url = Url::parse(reencode(filename));
        if (!url) {
            return nullptr;
        }

        // makeSource() rather than open(): a `.bnk` bank is not a track, and
        // asking the registry to *decode* it would be a different question.
        SourcePtr source = registry_->makeSource(*url);
        if (!source || !source->open(*url)) {
            return nullptr;
        }
        ISource* borrowed = source.get();
        return finish(std::make_unique<SourceStream>(borrowed, std::move(source)));
    }

    void close(binistream* stream) const override { delete stream; }

private:
    /// Puts a decoded URL back into a form Url::parse() accepts.
    ///
    /// Not simply percentEncodePath(): that escapes a path *component*, and a
    /// colon is not an unreserved path character -- so it turns `file:` into
    /// `file%3A` and the result does not parse at all. Sierra's SCI format is
    /// where this showed up, since it is the one in the corpus that reaches for
    /// a companion file, and the symptom was 120 files that would not open with
    /// their instrument bank sitting right beside them.
    ///
    /// So the scheme and its separator are held back and only what follows is
    /// encoded. Everything AdPlug asks for is a sibling of a URL this codec was
    /// given, so there is always a scheme; the fallback is for the case where
    /// a player invents a bare name.
    [[nodiscard]] static std::string reencode(const std::string& filename) {
        const std::size_t separator = filename.find("://");
        if (separator == std::string::npos) {
            return percentEncodePath(filename);
        }
        return filename.substr(0, separator + 3) +
               percentEncodePath(std::string_view{filename}.substr(separator + 3));
    }

    /// The two flags Cog sets, and they are not defaults: libbinio starts
    /// big-endian, and every format AdPlug reads came off a PC.
    [[nodiscard]] static binistream* finish(std::unique_ptr<SourceStream> stream) {
        if (stream->error() != 0) {
            return nullptr;
        }
        stream->setFlag(binio::BigEndian, false);
        stream->setFlag(binio::FloatIEEE);
        return stream.release();
    }

    std::string           primaryPath_;
    ISource*              primary_  = nullptr;
    const PluginRegistry* registry_ = nullptr;
};

/// The name to hand the factory: the URL, percent-decoded.
///
/// Decoded because the players do string surgery on it -- KSM replaces
/// everything after the last separator with `insts.dat`, ROL swaps the
/// extension -- and doing that to an encoded string produces a name that only
/// re-encodes correctly by luck. Cog decodes for the same reason and the
/// provider re-encodes on the way back.
[[nodiscard]] std::string factoryPath(const Url& url) {
    return percentDecode(url.withoutFragment().toString());
}

// ---------------------------------------------------------------------------
// Which extensions the linked library actually plays
// ---------------------------------------------------------------------------

/// Every extension every compiled-in player claims, asked of the library.
///
/// Cog builds this list on every call; here it is built once into function-local
/// storage that the registry's span then points at, because a descriptor's
/// extensions have to outlive registration. AdPlug states its extensions with
/// the dot and the registry matches without it, hence the strip.
///
/// **This must not run before main.** CAdPlug::players is a std::list of
/// CPlayerDesc, each holding a std::string and a heap buffer, and both it and
/// the array it is built from are filled by a dynamic initialiser in AdPlug's
/// own translation unit. Reading it from another unit's static initialiser is
/// the classic order fiasco and it is not detectable from here -- an
/// unconstructed std::list has no state that can be checked, so the symptom is
/// a segmentation fault at startup with no output at all. Registration happens
/// from main() everywhere in this tree; a test that registered from a
/// namespace-scope initialiser is what found this, and it is fixed there rather
/// than worked around here, because there is no working around it.
[[nodiscard]] std::span<const std::string_view> supportedExtensions() {
    static const std::vector<std::string> owned = [] {
        std::vector<std::string> list;
        for (const CPlayerDesc* player : CAdPlug::players) {
            for (unsigned i = 0; player->get_extension(i) != nullptr; ++i) {
                std::string extension{player->get_extension(i)};
                if (!extension.empty() && extension.front() == '.') {
                    extension.erase(0, 1);
                }
                if (extension.empty()) {
                    continue;
                }
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                list.push_back(std::move(extension));
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

/// Cog's `[[url fragment] intValue]`, which answers 0 for anything unparseable.
[[nodiscard]] unsigned subsongFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    unsigned               value    = 0;
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return fragment.empty() ? 0 : value;
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

class AdPlugDecoder final : public IDecoder {
public:
    ~AdPlugDecoder() override { AdPlugDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr || !source->seekable()) {
            // See the file comment: the factory re-reads the primary once per
            // player, and five players open a companion file. Neither is
            // something a stream can be asked for.
            return false;
        }
        adplug::installDatabase();

        rate_ = preferredRate(settings_);
        emu_  = std::make_unique<CNemuopl>(rate_);

        const Url& url  = source->url();
        const std::string path = factoryPath(url);

        player_.reset(CAdPlug::factory(path, emu_.get(), CAdPlug::players,
                                       SourceProvider{path, source, registry_}));
        if (!player_) {
            emu_.reset();
            return false;
        }

        subsong_ = subsongFromFragment(url);
        if (subsong_ >= player_->getsubsongs()) {
            subsong_ = 0;
        }

        // songlength() is in milliseconds and is measured by the player, which
        // for most formats means running the sequence through once.
        totalFrames_ = static_cast<std::int64_t>(
            player_->songlength(subsong_) * static_cast<double>(rate_) / 1000.0);

        readTags();

        player_->rewind(static_cast<int>(subsong_));
        pending_  = 0;
        framePos_ = 0;

        format_.sampleRate    = static_cast<double>(rate_);
        format_.channels      = kChannels;
        format_.channelConfig = 0x3;  // FL | FR
        format_.format        = SampleFormat::S16;
        format_.bitsPerSample = 16;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = codec_;
        props.encoding    = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

    bool readAudio(AudioChunk& out) override {
        if (!player_ || !emu_) {
            return false;
        }

        // Asked per read rather than latched at open, so switching repeat-one on
        // part-way through stops the track ending where the player said it would.
        const bool endless = loopForever(settings_);
        if (!endless && framePos_ >= totalFrames_) {
            return false;
        }

        auto want = static_cast<std::size_t>(kFramesPerRead);
        if (!endless) {
            want = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(want),
                                       totalFrames_ - framePos_));
        }
        if (want == 0) {
            return false;
        }

        scratch_.resize(want * kChannels);
        const std::size_t got = render(scratch_.data(), want, endless);
        if (got == 0) {
            return false;
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / static_cast<double>(rate_);
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(got);
        std::memcpy(dst, scratch_.data(), got * kChannels * sizeof(std::int16_t));
        out.setFrameCount(got);

        framePos_ += static_cast<std::int64_t>(got);
        return true;
    }

    /// Seeking is replaying, as it is for every tracker: the chip's state at any
    /// moment is every register write before it. Backwards means rewinding the
    /// subsong; forwards means running the sequence and discarding the audio.
    std::int64_t seek(std::int64_t frame) override {
        if (!player_) {
            return -1;
        }
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        if (frame < framePos_) {
            player_->rewind(static_cast<int>(subsong_));
            framePos_ = 0;
            pending_  = 0;
        }

        // update() alone, without emu_->update(), so nothing is synthesised for
        // the part being skipped -- Cog does the same, and here it is the reason
        // seeking a long tune is instant rather than a full render.
        while (framePos_ < frame) {
            player_->update();
            framePos_ += framesPerTick();
        }

        // The overshoot is kept rather than dropped: framePos_ has passed the
        // target by up to one tick, and those frames are real audio the caller
        // is about to ask for.
        pending_  = static_cast<std::size_t>(framePos_ - frame);
        framePos_ = frame;
        return frame;
    }

    void close() override {
        player_.reset();
        emu_.reset();
        scratch_.clear();
        pending_  = 0;
        framePos_ = 0;
    }

private:
    /// Frames between one player tick and the next.
    ///
    /// getrefresh() is a rate in hertz and it is *not* constant: a format with
    /// per-position tempo -- ROL and HERAD among them -- answers differently as
    /// the sequence runs, so this is asked every tick rather than once at open.
    [[nodiscard]] std::int64_t framesPerTick() const {
        // getrefresh() answers a float; widening it explicitly, because an
        // implicit promotion here is a warning on Clang and this tree keeps a
        // clean build on all four compilers.
        const auto refresh = static_cast<double>(player_->getrefresh());
        if (!(refresh > 0.0)) {
            return 1;
        }
        return static_cast<std::int64_t>(
            std::ceil(static_cast<double>(rate_) / refresh));
    }

    /// Fills `out`, driving the sequencer a tick at a time and carrying the
    /// remainder of the last tick across calls.
    [[nodiscard]] std::size_t render(std::int16_t* out, std::size_t frames, bool endless) {
        std::size_t filled = 0;
        while (filled < frames) {
            if (pending_ == 0) {
                const bool running = player_->update();
                if (!running && !endless) {
                    // The sequence ended earlier than songlength() promised.
                    // Better to stop than to synthesise silence up to a length
                    // that turned out to be wrong.
                    break;
                }
                pending_ = static_cast<std::size_t>(framesPerTick());
                if (pending_ == 0) {
                    break;
                }
            }

            const std::size_t take = std::min(frames - filled, pending_);
            emu_->update(out + (filled * kChannels), static_cast<int>(take));
            filled += take;
            pending_ -= take;
        }
        return filled;
    }

    void readTags() {
        const std::string title  = codecs::toUtf8(player_->gettitle());
        const std::string author = codecs::toUtf8(player_->getauthor());
        const std::string desc   = codecs::toUtf8(player_->getdesc());

        if (!title.empty()) {
            tags_.set("title", title);
        }
        if (!author.empty()) {
            tags_.set("artist", author);
        }
        // Cog reads only the title and the author. The description is where
        // several formats keep the tracker's own comment block, and dropping it
        // loses the only thing many of these files say about themselves.
        if (!desc.empty()) {
            tags_.set("comment", desc);
        }
        if (player_->getsubsongs() > 1) {
            tags_.set("track", std::to_string(subsong_ + 1));
        }

        codec_ = codecs::toUtf8(player_->gettype());
        if (codec_.empty()) {
            codec_ = "AdLib";
        }
    }

    const PluginRegistry* registry_ = nullptr;
    const Settings*       settings_ = nullptr;

    std::unique_ptr<CNemuopl> emu_;
    std::unique_ptr<CPlayer>  player_;

    AudioFormat  format_{};
    int          rate_        = kDefaultRate;
    unsigned     subsong_     = 0;
    std::int64_t totalFrames_ = 0;
    std::int64_t framePos_    = 0;

    /// Frames rendered by the last tick that the caller has not taken yet.
    std::size_t pending_ = 0;

    std::vector<std::int16_t> scratch_;
    std::string               codec_;
    MetadataMap               tags_;
};

/// A file holding several songs expands to one URL per song that plays.
///
/// Loaded through CSilentopl, which is AdPlug's own null OPL backend: the
/// factory has to run a player to learn what is in the file, and there is no
/// reason for it to synthesise anything while doing so.
///
/// **Empty subsongs are left out**, and that is not tidying. Westwood's ADL is
/// a bank of slots rather than a list of songs -- Dune II's DUNE20.adl declares
/// seventy-four and exactly half of them are zero seconds long. Cog offers all
/// seventy-four, so a playlist gains thirty-seven rows that select nothing,
/// report no duration and play no audio. Asking songlength() is what tells them
/// apart and the player is already loaded, so it costs a few milliseconds for
/// the whole file.
std::vector<Url> expandSongs(const Url& url, ISource& source,
                             const PluginRegistry& registry) {
    if (!url.fragment().empty() || !source.seekable()) {
        return {url};
    }
    adplug::installDatabase();

    CSilentopl       silent;
    const std::string path = factoryPath(url);

    const std::unique_ptr<CPlayer> player{
        CAdPlug::factory(path, &silent, CAdPlug::players,
                         SourceProvider{path, &source, &registry})};
    if (!player) {
        return {url};
    }

    const unsigned songs = player->getsubsongs();
    if (songs <= 1) {
        return {url};
    }

    std::vector<Url> tracks;
    tracks.reserve(songs);
    for (unsigned i = 0; i < songs; ++i) {
        if (player->songlength(i) > 0) {
            tracks.push_back(url.withFragment(std::to_string(i)));
        }
    }

    // A file whose songs are *all* empty stays one row rather than none. It is
    // almost certainly not playable, but dropping it silently would leave a
    // file the user asked for missing from the playlist with nothing said.
    if (tracks.empty()) {
        return {url};
    }
    return tracks;
}

}  // namespace
}  // namespace xpcog

void xpcog_register_adplug(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "AdPlugContainer",
        .priority   = xpcog::kAdPlugPriority,
        .extensions = xpcog::supportedExtensions(),
        .mimeTypes  = {},
        .expand     = &xpcog::expandSongs,
    });

    r.addDecoder({
        .name       = "AdPlugDecoder",
        .priority   = xpcog::kAdPlugPriority,
        .extensions = xpcog::supportedExtensions(),
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::AdPlugDecoder>();
        },
        .available = nullptr,
    });
}
