// Tags that arrive mid-stream, as ID3v2 chunks spliced between audio frames.
//
// This is how a live stream renames the track that is playing when the transport
// is not SHOUTcast. An HLS packed-audio rendition carries an ID3v2 tag at the
// head of every segment, and the ones after the first are the station saying
// what is on now -- so for a stream reaching XPCog through the HLS decoder, this
// is the only path a now-playing title has.
//
// FFmpeg's ADTS demuxer has parsed these into AVFormatContext::metadata and
// raised AVFMT_EVENT_FLAG_METADATA_UPDATED for years. Nothing here read the
// flag, so every tag after the first was decoded and thrown away, and the
// failure was silent: audio played, the title just never changed.
//
// The fixture is built by splicing tags into a real ADTS stream at a real frame
// boundary, because that is the case the demuxer distinguishes -- a tag at a
// byte offset that is not a frame start makes it resync and drop the tag
// instead, which would pass a test that only checked "some metadata arrived".

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
}

#include "../TestShell.hpp"
#include "../TestSignal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int    kFrames     = 44100 * 4;

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-ffmpeg-tag-tests";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    REQUIRE(f != nullptr);
    std::vector<std::uint8_t> data;
    std::uint8_t              buffer[4096];
    std::size_t               got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        data.insert(data.end(), buffer, buffer + got);
    }
    std::fclose(f);
    return data;
}

std::filesystem::path referenceWav() {
    const auto out = fixtureDir() / "reference.wav";
    if (std::filesystem::exists(out)) {
        return out;
    }

    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(kFrames) * 2);
    for (int i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        samples.push_back(static_cast<std::int16_t>(
            0.67 * 32767.0 * std::sin(xpcog::test::kTwoPi * 440.0 * t)));
        samples.push_back(static_cast<std::int16_t>(
            0.55 * 32767.0 * std::sin(xpcog::test::kTwoPi * 660.0 * t)));
    }

    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
    std::FILE* f         = std::fopen(out.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2);
    u32(static_cast<std::uint32_t>(kSampleRate));
    u32(static_cast<std::uint32_t>(kSampleRate) * 4);
    u16(4); u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(samples.data(), 1, dataBytes, f);
    std::fclose(f);
    return out;
}

void appendSyncsafe(std::vector<std::uint8_t>& out, std::size_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 21) & 0x7F));
    out.push_back(static_cast<std::uint8_t>((value >> 14) & 0x7F));
    out.push_back(static_cast<std::uint8_t>((value >> 7) & 0x7F));
    out.push_back(static_cast<std::uint8_t>(value & 0x7F));
}

/// One ID3v2.4 frame, header included.
std::vector<std::uint8_t> id3v2Frame(std::string_view identifier,
                                     const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame;
    frame.insert(frame.end(), identifier.begin(), identifier.end());
    appendSyncsafe(frame, payload.size());
    frame.insert(frame.end(), {0x00, 0x00});  // frame flags
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::uint8_t> id3v2TextFrame(std::string_view identifier,
                                         std::string_view text) {
    std::vector<std::uint8_t> payload{0x03};  // UTF-8
    payload.insert(payload.end(), text.begin(), text.end());
    return id3v2Frame(identifier, payload);
}

/// The PRIV frame every HLS segment carries, whose value is that segment's own
/// timestamp and therefore differs on every one.
std::vector<std::uint8_t> id3v2TimestampFrame(std::uint64_t timestamp) {
    const std::string         owner = "com.apple.streaming.transportStreamTimestamp";
    std::vector<std::uint8_t> payload(owner.begin(), owner.end());
    payload.push_back(0x00);
    for (int i = 7; i >= 0; --i) {
        payload.push_back(static_cast<std::uint8_t>((timestamp >> (8 * i)) & 0xFF));
    }
    return id3v2Frame("PRIV", payload);
}

/// An ID3v2.4 tag around the given frames. Sizes are syncsafe -- seven bits per
/// byte, high bit always clear, so a tag can never be mistaken for an audio sync
/// word.
std::vector<std::uint8_t> id3v2Tag(const std::vector<std::vector<std::uint8_t>>& frames) {
    std::vector<std::uint8_t> body;
    for (const auto& frame : frames) {
        body.insert(body.end(), frame.begin(), frame.end());
    }

    std::vector<std::uint8_t> tag;
    tag.insert(tag.end(), {'I', 'D', '3', 0x04, 0x00, 0x00});
    appendSyncsafe(tag, body.size());
    tag.insert(tag.end(), body.begin(), body.end());
    return tag;
}

/// The shape a packed-audio segment leads with, reduced to the one frame most
/// of these tests care about.
std::vector<std::uint8_t> id3v2TitleTag(std::string_view title) {
    return id3v2Tag({id3v2TextFrame("TIT2", title)});
}

/// What a real station sends: the title repeated in every segment, each tag
/// carrying several stable frames alongside a timestamp that moves.
std::vector<std::uint8_t> id3v2BroadcastTag(std::string_view artist,
                                            std::string_view title,
                                            std::uint64_t    timestamp) {
    return id3v2Tag({id3v2TextFrame("TPE1", artist), id3v2TextFrame("TIT2", title),
                     id3v2TextFrame("TALB", "Live"), id3v2TimestampFrame(timestamp)});
}

/// The offset of the first ADTS frame that starts at or after `at`. Walking the
/// frame-length field rather than searching for a sync word: 0xFFF occurs inside
/// compressed audio too, and splicing a tag into the middle of a frame is
/// precisely the case the demuxer throws away.
std::size_t adtsFrameBoundary(const std::vector<std::uint8_t>& data, std::size_t at) {
    std::size_t position = 0;

    // Skip the leading ID3v2 tag the encoder may have written.
    if (data.size() > 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        std::size_t size = 0;
        for (std::size_t i = 6; i < 10; ++i) {
            size = (size << 7) | (data[i] & 0x7FU);
        }
        position = size + 10;
    }

    while (position + 7 <= data.size()) {
        if (data[position] != 0xFF || (data[position + 1] & 0xF0) != 0xF0) {
            return 0;
        }
        if (position >= at) {
            return position;
        }
        const std::size_t length = (static_cast<std::size_t>(data[position + 3] & 0x03) << 11) |
                                   (static_cast<std::size_t>(data[position + 4]) << 3) |
                                   (static_cast<std::size_t>(data[position + 5]) >> 5);
        if (length < 7) {
            return 0;
        }
        position += length;
    }
    return 0;
}

/// A raw ADTS stream with `Opening Number` at the front and `Second Number`
/// spliced in at the frame boundary nearest the middle. Empty when ffmpeg is not
/// installed to produce the audio.
std::filesystem::path buildTaggedStream() {
    const auto tagged = fixtureDir() / "tagged.aac";
    if (std::filesystem::exists(tagged)) {
        return tagged;
    }
    if (!xpcog::test::haveTool("ffmpeg")) {
        return {};
    }

    const auto        plain   = fixtureDir() / "plain.aac";
    const std::string command = "ffmpeg -y -loglevel error -i \"" +
                                referenceWav().string() +
                                "\" -c:a aac -b:a 128k -f adts \"" + plain.string() +
                                "\"" + xpcog::test::kSilenceStderr;
    if (std::system(command.c_str()) != 0 || !std::filesystem::exists(plain)) {
        return {};
    }

    const std::vector<std::uint8_t> audio = readBytes(plain);
    REQUIRE(audio.size() > 1024);

    const std::size_t split = adtsFrameBoundary(audio, audio.size() / 2);
    REQUIRE(split > 0);
    REQUIRE(split < audio.size());

    const auto first  = id3v2TitleTag("Opening Number");
    const auto second = id3v2TitleTag("Second Number");

    std::vector<std::uint8_t> out;
    out.insert(out.end(), first.begin(), first.end());
    out.insert(out.end(), audio.begin(), audio.begin() + static_cast<std::ptrdiff_t>(split));
    out.insert(out.end(), second.begin(), second.end());
    out.insert(out.end(), audio.begin() + static_cast<std::ptrdiff_t>(split), audio.end());

    writeBytes(tagged, out);
    return tagged;
}

/// The same stream cut into four segments at real frame boundaries, each led by
/// its own ID3v2 tag, with a manifest beside them -- which is exactly the shape
/// of an HLS packed-audio rendition. Empty when the fixture cannot be built.
std::filesystem::path buildTaggedHlsStream() {
    const auto manifest = fixtureDir() / "tagged.m3u8";
    if (std::filesystem::exists(manifest)) {
        return manifest;
    }
    if (buildTaggedStream().empty()) {
        return {};
    }

    const std::vector<std::uint8_t> audio = readBytes(fixtureDir() / "plain.aac");
    REQUIRE(audio.size() > 4096);

    constexpr int     kSegments = 4;
    std::string       playlist  = "#EXTM3U\n#EXT-X-TARGETDURATION:1\n"
                                  "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n";
    std::size_t       begin     = adtsFrameBoundary(audio, 0);
    REQUIRE(begin < audio.size());

    for (int i = 0; i < kSegments; ++i) {
        const std::size_t wanted = audio.size() * static_cast<std::size_t>(i + 1) / kSegments;
        const std::size_t end =
            (i + 1 == kSegments) ? audio.size() : adtsFrameBoundary(audio, wanted);
        REQUIRE(end > begin);

        const auto                tag = id3v2TitleTag("Track " + std::to_string(i));
        std::vector<std::uint8_t> segment(tag);
        segment.insert(segment.end(), audio.begin() + static_cast<std::ptrdiff_t>(begin),
                       audio.begin() + static_cast<std::ptrdiff_t>(end));

        const std::string name = "seg" + std::to_string(i) + ".aac";
        writeBytes(fixtureDir() / name, segment);
        playlist += "#EXTINF:1,\n" + name + "\n";
        begin = end;
    }
    playlist += "#EXT-X-ENDLIST\n";

    std::vector<std::uint8_t> bytes(playlist.size());
    std::memcpy(bytes.data(), playlist.data(), playlist.size());
    writeBytes(manifest, bytes);
    return manifest;
}

/// Remuxes the ADTS fixture into MPEG-TS with a second elementary stream of
/// AV_CODEC_ID_TIMED_ID3, carrying one complete ID3v2 tag at each of `at`
/// seconds. Empty when the fixture cannot be built.
///
/// Built with libavformat rather than the ffmpeg binary because there is no way
/// to ask the CLI for this: only the mpegts demuxer ever produces a timed-ID3
/// stream, so nothing exists to feed the muxer from.
std::filesystem::path buildTimedId3Ts(
    const std::vector<std::pair<double, std::string>>& at) {
    const auto out = fixtureDir() / "timed.ts";
    if (std::filesystem::exists(out)) {
        return out;
    }
    if (buildTaggedStream().empty()) {
        return {};
    }
    const auto source = fixtureDir() / "plain.aac";

    // The decoder quiets FFmpeg when it first opens something, but this runs
    // before that and would otherwise print muxer diagnostics into the test log.
    av_log_set_level(AV_LOG_QUIET);

    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, source.string().c_str(), nullptr, nullptr) < 0) {
        return {};
    }
    if (avformat_find_stream_info(input, nullptr) < 0) {
        avformat_close_input(&input);
        return {};
    }
    const int audioIn = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIn < 0) {
        avformat_close_input(&input);
        return {};
    }

    AVFormatContext* output = nullptr;
    if (avformat_alloc_output_context2(&output, nullptr, "mpegts",
                                       out.string().c_str()) < 0 ||
        output == nullptr) {
        avformat_close_input(&input);
        return {};
    }

    AVStream* audioOut = avformat_new_stream(output, nullptr);
    avcodec_parameters_copy(audioOut->codecpar, input->streams[audioIn]->codecpar);
    audioOut->codecpar->codec_tag = 0;

    AVStream* metaOut               = avformat_new_stream(output, nullptr);
    metaOut->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    metaOut->codecpar->codec_id   = AV_CODEC_ID_TIMED_ID3;

    if (avio_open(&output->pb, out.string().c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(output, nullptr) < 0) {
        avformat_close_input(&input);
        avformat_free_context(output);
        return {};
    }

    AVPacket*   packet = av_packet_alloc();
    std::size_t next   = 0;
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index != audioIn) {
            av_packet_unref(packet);
            continue;
        }

        const AVRational sourceBase = input->streams[audioIn]->time_base;
        const double     seconds    = static_cast<double>(packet->pts) * av_q2d(sourceBase);

        while (next < at.size() && seconds >= at[next].first) {
            const auto                tagBytes = id3v2TitleTag(at[next].second);
            AVPacket*                 tag      = av_packet_alloc();
            av_new_packet(tag, static_cast<int>(tagBytes.size()));
            std::memcpy(tag->data, tagBytes.data(), tagBytes.size());
            tag->stream_index = metaOut->index;
            tag->pts = tag->dts =
                av_rescale_q(packet->pts, sourceBase, metaOut->time_base);
            tag->flags |= AV_PKT_FLAG_KEY;
            av_interleaved_write_frame(output, tag);
            av_packet_free(&tag);
            ++next;
        }

        av_packet_rescale_ts(packet, sourceBase, audioOut->time_base);
        packet->stream_index = audioOut->index;
        av_interleaved_write_frame(output, packet);
        av_packet_unref(packet);
    }

    av_write_trailer(output);
    av_packet_free(&packet);
    avio_closep(&output->pb);
    avformat_free_context(output);
    avformat_close_input(&input);

    return std::filesystem::exists(out) ? out : std::filesystem::path{};
}

const bool kHaveHls = [] {
    const auto extensions = registry().allExtensions();
    return std::find(extensions.begin(), extensions.end(), "m3u8") != extensions.end();
}();

const bool kHaveFFmpeg = [] {
    const auto extensions = registry().allExtensions();
    return std::find(extensions.begin(), extensions.end(), "aac") != extensions.end();
}();

}  // namespace

TEST_CASE("a mid-stream ID3v2 chunk updates the tags", "[ffmpeg][streamtags]") {
    if (!kHaveFFmpeg) SKIP("the FFmpeg decoder is not built into this configuration");

    const auto path = buildTaggedStream();
    if (path.empty()) SKIP("ffmpeg not available to build an ADTS fixture");

    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);

    // The tag at the head of the stream is a header, not an update, and must not
    // be announced as a change -- a chain that reacts to it would treat the first
    // read of every file as a track rename.
    CHECK(opened.decoder->metadata().first("title") == "Opening Number");

    int         announcements    = 0;
    bool        propertiesMoved  = false;
    std::size_t framesAtFirstTag = 0;
    std::size_t frames           = 0;
    std::string latestTitle;

    opened.decoder->setChangeCallback(
        [&](bool propertiesChanged, bool metadataChanged) {
            if (!metadataChanged) {
                return;
            }
            propertiesMoved = propertiesMoved || propertiesChanged;
            if (announcements == 0) {
                framesAtFirstTag = frames;
            }
            ++announcements;
            latestTitle = opened.decoder->metadata().first("title");
        });

    AudioChunk chunk;
    while (opened.decoder->readAudio(chunk)) {
        frames += chunk.frameCount();
    }

    REQUIRE(frames > 0);
    // Exactly one: the flag is consumed when it is read, so a spliced tag is
    // reported once rather than on every packet after it.
    CHECK(announcements == 1);
    CHECK(latestTitle == "Second Number");
    // A tag block does not change the format, and saying it did makes the chain
    // re-evaluate the stream for nothing.
    CHECK_FALSE(propertiesMoved);

    // Reported where it appears, not at the end. Roughly halfway, generously
    // bounded because AAC's decoder delay and the demuxer's read-ahead both put
    // the announcement a few frames off the splice.
    CHECK(framesAtFirstTag > frames / 8);
    CHECK(framesAtFirstTag < frames * 7 / 8);
}

TEST_CASE("a stream with no mid-stream tags announces nothing", "[ffmpeg][streamtags]") {
    if (!kHaveFFmpeg) SKIP("the FFmpeg decoder is not built into this configuration");

    // The other half of the check: harvesting on every packet instead of on the
    // demuxer's flag would also make the test above pass, while reporting a track
    // rename thousands of times a second for every ordinary file.
    const auto tagged = buildTaggedStream();
    if (tagged.empty()) SKIP("ffmpeg not available to build an ADTS fixture");

    const auto plain = fixtureDir() / "plain.aac";
    REQUIRE(std::filesystem::exists(plain));

    auto opened = registry().open(Url::fromLocalPath(plain));
    REQUIRE(opened);

    int announcements = 0;
    opened.decoder->setChangeCallback([&](bool, bool metadataChanged) {
        if (metadataChanged) {
            ++announcements;
        }
    });

    AudioChunk chunk;
    while (opened.decoder->readAudio(chunk)) {
    }
    CHECK(announcements == 0);
}

TEST_CASE("an HLS rendition's per-segment tags reach the playlist",
          "[ffmpeg][streamtags][hls]") {
    if (!kHaveFFmpeg) SKIP("the FFmpeg decoder is not built into this configuration");
    if (!kHaveHls) SKIP("HLS is not built into this configuration");

    // The whole path, which is what the feature is for: the fetcher concatenates
    // segments, each segment leads with its own ID3v2 tag, the ADTS demuxer
    // parses those as mid-stream updates, and the HLS decoder forwards them on
    // behalf of the decoder it wrapped. Any link missing and the title never
    // changes -- while the audio plays perfectly, which is why this is a test
    // rather than something anyone would notice.
    const auto manifest = buildTaggedHlsStream();
    if (manifest.empty()) SKIP("ffmpeg not available to build an HLS fixture");

    auto opened = registry().open(Url::fromLocalPath(manifest));
    REQUIRE(opened);

    std::vector<std::string> observed;
    observed.emplace_back(opened.decoder->metadata().first("title"));
    opened.decoder->setChangeCallback([&](bool, bool metadataChanged) {
        if (metadataChanged) {
            observed.emplace_back(opened.decoder->metadata().first("title"));
        }
    });

    AudioChunk  chunk;
    std::size_t frames = 0;
    while (opened.decoder->readAudio(chunk)) {
        frames += chunk.frameCount();
    }
    REQUIRE(frames > 0);

    // A suffix rather than the whole list, because the fetcher runs ahead and
    // avformat_open_input probes into what it has already queued -- so by the
    // time open() returns, the tag on view can be the second segment's. That
    // head start is bounded by the probe and is FFmpeg's behaviour for any
    // stream, not something the HLS layer introduces.
    //
    // What must hold is everything after it: in order, none dropped, none
    // repeated. A station announcing them out of sequence leaves the wrong track
    // named, and a dropped one leaves the previous title standing for two
    // segments -- both of which look like the feature working.
    const std::vector<std::string> expected{"Track 0", "Track 1", "Track 2", "Track 3"};
    REQUIRE(observed.size() >= expected.size() - 1);
    REQUIRE(observed.size() <= expected.size());
    CHECK(std::equal(observed.begin(), observed.end(),
                     expected.end() - static_cast<std::ptrdiff_t>(observed.size())));
}

TEST_CASE("MPEG-TS timed metadata renames the playing track",
          "[ffmpeg][streamtags]") {
    if (!kHaveFFmpeg) SKIP("the FFmpeg decoder is not built into this configuration");

    // The other way HLS carries a now-playing title. A packed-audio rendition
    // splices ID3 into the audio, where the ADTS demuxer parses it; a transport
    // stream puts it in an elementary stream of its own (stream_type 0x15),
    // where libavformat hands over the bytes and parses nothing -- so this is
    // the path that needs our own ID3 reader.
    const auto path = buildTimedId3Ts({{1.0, "Second Number"}, {2.5, "Third Number"}});
    if (path.empty()) SKIP("ffmpeg not available to build an MPEG-TS fixture");

    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);

    std::vector<std::string> titles;
    opened.decoder->setChangeCallback([&](bool propertiesChanged, bool metadataChanged) {
        CHECK_FALSE(propertiesChanged);
        if (metadataChanged) {
            titles.emplace_back(opened.decoder->metadata().first("title"));
        }
    });

    AudioChunk  chunk;
    std::size_t frames = 0;
    while (opened.decoder->readAudio(chunk)) {
        frames += chunk.frameCount();
    }
    REQUIRE(frames > 0);

    // Unlike the in-band case, these arrive as ordinary packets, so they are
    // reported as the stream is read rather than during the probe.
    CHECK(titles == std::vector<std::string>{"Second Number", "Third Number"});
}

TEST_CASE("MPEG-TS timed metadata that repeats is announced once",
          "[ffmpeg][streamtags]") {
    if (!kHaveFFmpeg) SKIP("the FFmpeg decoder is not built into this configuration");

    // Every HLS transport-stream segment carries a timed-ID3 packet whether or
    // not the programme moved on -- with the Apple timestamp PRIV frame in it,
    // whose value changes every time. A reader that announced each packet, or
    // that kept PRIV, would rename the track every few seconds for the whole
    // broadcast.
    std::filesystem::remove(fixtureDir() / "timed.ts");
    const auto path = buildTimedId3Ts({{0.5, "Only Track"},
                                       {1.0, "Only Track"},
                                       {1.5, "Only Track"},
                                       {2.0, "Only Track"}});
    if (path.empty()) SKIP("ffmpeg not available to build an MPEG-TS fixture");

    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);

    int announcements = 0;
    opened.decoder->setChangeCallback([&](bool, bool metadataChanged) {
        if (metadataChanged) {
            ++announcements;
        }
    });

    AudioChunk chunk;
    while (opened.decoder->readAudio(chunk)) {
    }
    CHECK(announcements == 1);

    // Left behind for any test that runs after this one in the same directory.
    std::filesystem::remove(fixtureDir() / "timed.ts");
}

TEST_CASE("a broadcast that repeats its title is announced once", "[ffmpeg][streamtags]") {
    if (!kHaveFFmpeg) SKIP("the FFmpeg decoder is not built into this configuration");

    // Taken from a real station, whose segments each lead with the same artist,
    // title and album plus Apple's per-segment timestamp. Nothing about the
    // programme has changed, but two separate things make it look as though it
    // has, and either one alone renames the playing track every ten seconds for
    // the length of the broadcast -- repainting the playlist row and refiring
    // every now-playing surface each time:
    //
    //   * the timestamp PRIV differs on every segment, so the tags genuinely are
    //     not byte-identical;
    //   * FFmpeg's av_dict replaces a value by swapping its last element into
    //     the vacated slot, so re-reading an unchanged dictionary yields the
    //     same tags rotated by one.
    //
    // Hence dropping that one PRIV, and comparing content rather than position.
    const auto path = fixtureDir() / "broadcast.aac";
    if (buildTaggedStream().empty()) SKIP("ffmpeg not available to build an ADTS fixture");

    const std::vector<std::uint8_t> audio = readBytes(fixtureDir() / "plain.aac");
    constexpr int                   kSegments = 5;

    std::vector<std::uint8_t> out;
    std::size_t               begin = adtsFrameBoundary(audio, 0);
    REQUIRE(begin < audio.size());

    for (int i = 0; i < kSegments; ++i) {
        const std::size_t wanted = audio.size() * static_cast<std::size_t>(i + 1) / kSegments;
        const std::size_t end =
            (i + 1 == kSegments) ? audio.size() : adtsFrameBoundary(audio, wanted);
        REQUIRE(end > begin);

        // The last segment is the only one where the programme actually moves on.
        const bool changed = (i + 1 == kSegments);
        const auto tag     = id3v2BroadcastTag(changed ? "Second Artist" : "First Artist",
                                               changed ? "Second Song" : "First Song",
                                               0x0d0000ULL + static_cast<std::uint64_t>(i) * 0x1000ULL);
        out.insert(out.end(), tag.begin(), tag.end());
        out.insert(out.end(), audio.begin() + static_cast<std::ptrdiff_t>(begin),
                   audio.begin() + static_cast<std::ptrdiff_t>(end));
        begin = end;
    }
    writeBytes(path, out);

    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);
    CHECK(opened.decoder->metadata().first("title") == "First Song");
    // Dropped rather than shown: it names nothing a listener reads, and it is
    // the reason an unchanging broadcast looks like it is changing.
    CHECK_FALSE(opened.decoder->metadata().contains(
        "id3v2_priv.com.apple.streaming.transportstreamtimestamp"));

    std::vector<std::string> titles;
    opened.decoder->setChangeCallback([&](bool, bool metadataChanged) {
        if (metadataChanged) {
            titles.emplace_back(opened.decoder->metadata().first("title"));
        }
    });

    AudioChunk chunk;
    while (opened.decoder->readAudio(chunk)) {
    }

    CHECK(titles == std::vector<std::string>{"Second Song"});
    CHECK(opened.decoder->metadata().first("artist") == "Second Artist");
}
