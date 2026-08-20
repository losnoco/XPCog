// Organya (.org) -- Cave Story's music format.
//
// Unusually for a music format, almost everything here runs without a corpus.
// An .org file is a header and five parallel arrays per track, all of it
// documented and none of it compressed, so a valid song can be built in the
// test and the interesting properties asserted against a file whose contents
// are known exactly. The corpus cases add breadth over real songs; they are not
// where the claims live.
//
// Two claims carry their weight.
//
// The first is that **seeking is exact**. A sequencer has no random access --
// the state at any moment is every note before it -- so the usual answer is
// either to replay the gap (slow) or to jump and accept that notes held across
// the seek point vanish (which is what Cog does). This decoder skips the
// mixing but does the same state arithmetic, which means audio after a seek
// should be *bit-identical* to the same audio decoded straight through. That is
// a much stronger assertion than "the position is about right", and it fails
// the moment anyone reintroduces the jump.
//
// The second is that **the length agrees with the audio**. The format counts
// beats in whole milliseconds and the mixer renders them in whole samples, and
// the conversion truncates. Cog derives the track length from the milliseconds
// while rendering from the samples, so the two drift apart by half a sample per
// beat on any song whose beat is not a whole number of frames. The synthetic
// song below has exactly such a beat, and the expected frame count is written
// out longhand.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

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
namespace fs = std::filesystem;

namespace {

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

#ifdef XPCOG_ORGANYA_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_ORGANYA_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] fs::path corpusRoot() { return {}; }
#endif

[[nodiscard]] std::vector<fs::path> findSongs(std::size_t want) {
    std::vector<fs::path> found;
    if (!kHaveCorpus) {
        return found;
    }
    std::error_code                  error;
    fs::recursive_directory_iterator walk{
        corpusRoot(), fs::directory_options::skip_permission_denied, error};
    if (error) {
        return found;
    }
    for (const fs::directory_entry& entry : walk) {
        if (found.size() >= want) {
            break;
        }
        if (!entry.is_regular_file(error)) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".org") {
            found.push_back(entry.path());
        }
    }
    return found;
}

// --- building a song -------------------------------------------------------

void putByte(std::string& out, int value) {
    out.push_back(static_cast<char>(value & 0xFF));
}
void putWord(std::string& out, int value) {
    putByte(out, value);
    putByte(out, value >> 8);
}
void putDword(std::string& out, int value) {
    putWord(out, value);
    putWord(out, value >> 16);
}

struct Note {
    int beat    = 0;
    int note    = 48;
    int length  = 1;
    int volume  = 200;
    int panning = 6;
};

/// The bytes of a valid Org-02 file: a header, sixteen instrument descriptors,
/// and then each track's events as five arrays -- beat numbers, then notes,
/// then lengths, then volumes, then pannings, each complete before the next
/// begins. That last part is the only thing about the layout worth being
/// careful with, and writing it here is what makes the reader's version of it
/// something a test can be wrong about.
[[nodiscard]] std::string makeOrg(int msPerBeat, int loopStart, int loopEnd,
                                  const std::vector<Note>& melody,
                                  const std::vector<Note>& drums = {}) {
    std::string out = "Org-02";
    putWord(out, msPerBeat);
    putByte(out, 4);  // steps per bar -- display only
    putByte(out, 4);  // beats per step -- display only
    putDword(out, loopStart);
    putDword(out, loopEnd);

    // Track 0 carries the melody; track 8 is the first percussion track.
    for (int track = 0; track < 16; ++track) {
        putWord(out, 0);  // tuning, in thousandths of a semitone
        putByte(out, 0);  // waveform (melodic) or drum slot (percussive)
        putByte(out, 0);  // "pi": fixed-length notes
        const std::size_t count = track == 0    ? melody.size()
                                  : track == 8  ? drums.size()
                                                : 0;
        putWord(out, static_cast<int>(count));
    }

    static const std::vector<Note> none;
    for (int track = 0; track < 16; ++track) {
        const std::vector<Note>& events = track == 0   ? melody
                                          : track == 8 ? drums
                                                       : none;
        for (const Note& note : events) {
            putDword(out, note.beat);
        }
        for (const Note& note : events) {
            putByte(out, note.note);
        }
        for (const Note& note : events) {
            putByte(out, note.length);
        }
        for (const Note& note : events) {
            putByte(out, note.volume);
        }
        for (const Note& note : events) {
            putByte(out, note.panning);
        }
    }
    return out;
}

[[nodiscard]] Url writeTemp(const std::string& name, const std::string& bytes) {
    const fs::path  dir = fs::temp_directory_path() / "xpcog-organya-tests";
    std::error_code error;
    fs::create_directories(dir, error);
    const fs::path path = dir / name;

    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file != nullptr) {
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
    }
    return Url::fromLocalPath(path);
}

// --- decoding --------------------------------------------------------------

struct Decoded {
    std::vector<float> samples;
    TrackProperties    properties;
};

[[nodiscard]] std::vector<float> drain(IDecoder& decoder, std::size_t limitSamples) {
    std::vector<float> samples;
    AudioChunk         chunk;
    while (samples.size() < limitSamples && decoder.readAudio(chunk)) {
        const std::size_t frames = chunk.frameCount();
        if (frames == 0) {
            break;
        }
        const std::size_t channels = chunk.format().channels;
        const std::size_t at       = samples.size();
        samples.resize(at + (frames * channels));
        std::memcpy(samples.data() + at, chunk.bytes().data(),
                    frames * channels * sizeof(float));
    }
    return samples;
}

[[nodiscard]] Decoded decode(const Url& url, std::size_t limitSamples) {
    Decoded                    out;
    PluginRegistry::OpenResult opened = registry().open(url);
    if (!opened) {
        return out;
    }
    out.properties = opened.decoder->properties();
    out.samples    = drain(*opened.decoder, limitSamples);
    return out;
}

[[nodiscard]] float peak(const std::vector<float>& samples) {
    float highest = 0.0F;
    for (const float sample : samples) {
        highest = std::max(highest, std::abs(sample));
    }
    return highest;
}

/// A song whose beat is deliberately *not* a whole number of frames: 125 ms at
/// 44,100 Hz is 5,512.5 samples, so the truncation this format forces on the
/// mixer is half a sample every beat.
constexpr int kMsPerBeat = 125;
constexpr int kLoopStart = 8;
constexpr int kLoopEnd   = 40;
/// trunc(125 * 44100 / 1000). Written out rather than computed so that changing
/// how the decoder rounds has to change this line too.
constexpr std::int64_t kFramesPerBeat = 5512;

[[nodiscard]] std::string tuneBytes() {
    std::vector<Note> melody;
    for (int beat = 0; beat < kLoopEnd; beat += 2) {
        melody.push_back(Note{beat, 40 + (beat % 12), 2, 220, 6});
    }
    std::vector<Note> drums;
    for (int beat = 0; beat < kLoopEnd; beat += 4) {
        drums.push_back(Note{beat, 60, 1, 220, 6});
    }
    return makeOrg(kMsPerBeat, kLoopStart, kLoopEnd, melody, drums);
}

}  // namespace

TEST_CASE("the Organya decoder claims .org", "[organya]") {
    CHECK(registry().isPlayableExtension("org"));
}

TEST_CASE("something that is not an Organya song is declined", "[organya]") {
    CHECK_FALSE(registry().open(writeTemp("garbage.org", "not a Cave Story song")));
    CHECK_FALSE(registry().open(writeTemp("empty.org", "")));

    // The signature alone is not enough: the header and sixteen instrument
    // descriptors are 118 bytes, and a file that stops inside them is where a
    // trusting loader reads past the end and invents a song out of whatever
    // followed it in memory.
    CHECK_FALSE(registry().open(writeTemp("headeronly.org", "Org-02")));

    // A complete header whose event counts describe events that are not there.
    // The counts are 16-bit, so they cannot ask for an absurd allocation -- what
    // they can do is ask for 65,535 events out of a file holding none.
    std::string lying = tuneBytes().substr(0, 118);
    CHECK_FALSE(registry().open(writeTemp("truncated.org", lying)));
}

TEST_CASE("a song renders audio, not silence", "[organya]") {
    const Decoded decoded = decode(writeTemp("tune.org", tuneBytes()), 44100 * 2);
    REQUIRE_FALSE(decoded.samples.empty());

    CHECK(decoded.properties.format.sampleRate == 44100.0);
    CHECK(decoded.properties.format.channels == 2);
    CHECK(decoded.properties.format.format == SampleFormat::F32);
    CHECK(decoded.properties.codec == "Organya");
    CHECK(decoded.properties.seekable);

    // A loader that parsed the header and then found no events emits perfect
    // silence, so the level is what separates "it parsed" from "it played".
    // The upper bound is loose on purpose: the player's master volume is 4e-6
    // against sixteen tracks of signed bytes summed without headroom, and a
    // dense song genuinely exceeds unity -- over a forty-song sample of Cave
    // Story's own music the median peak is 0.95 and sixteen of them pass 1.0,
    // the loudest at 1.62. That is Bisqwit's scale factor and Cog's, and
    // clamping it here would throw away headroom the float format carries.
    const float level = peak(decoded.samples);
    INFO("peak " << level);
    CHECK(level > 0.01F);
    CHECK(level < 8.0F);
}

TEST_CASE("the length is the beats that are actually rendered", "[organya]") {
    PluginRegistry::OpenResult opened = registry().open(writeTemp("tune.org", tuneBytes()));
    REQUIRE(opened);

    // The intro once, then the loop twice -- the default loop count -- and each
    // beat is trunc(ms * rate / 1000) frames rather than ms * rate / 1000.
    //
    // Deriving it the other way, which is what Cog does, gives
    // ceil(0.125 * 72 * 44100) = 396,900 against the 396,864 that come out: 36
    // frames of a track that does not exist. Small, and it grows with the song.
    constexpr std::int64_t kBeats = kLoopStart + ((kLoopEnd - kLoopStart) * 2);
    constexpr std::int64_t kBody  = kBeats * kFramesPerBeat;
    // The default fade, ceil()ed to frames.
    constexpr std::int64_t kFade = 8 * 44100;

    CHECK(opened.decoder->properties().totalFrames == kBody + kFade);

    // And the decoder delivers exactly that many, rather than reporting one
    // number and producing another.
    const std::vector<float> samples = drain(*opened.decoder, 0xFFFFFFFF);
    CHECK(static_cast<std::int64_t>(samples.size() / 2) == kBody + kFade);

    // The tail is the fade, so the last frames are near silence whatever the
    // song was doing when it got there.
    REQUIRE(samples.size() > 32);
    const std::vector<float> tail{samples.end() - 32, samples.end()};
    CHECK(peak(tail) < 0.01F);
}

TEST_CASE("seeking is exact, not approximate", "[organya]") {
    const Url url = writeTemp("tune.org", tuneBytes());

    // Straight through, as the reference.
    constexpr std::size_t kFrames = 44100 * 10;
    const Decoded straight = decode(url, kFrames * 2);
    REQUIRE(straight.samples.size() >= kFrames * 2);

    // Deliberately not on a beat boundary, and deliberately past the loop
    // point: the song runs beats 0..39 and then 8..39 for ever, so beat 50 is
    // the eleventh beat of the second pass. A beat is 5,512 frames, so this
    // lands 1,234 frames into it -- which is where a decoder that seeks by
    // whole beats and discards the remainder loses those 1,234 frames' worth of
    // any note that starts on that beat.
    constexpr std::int64_t kTarget = (kFramesPerBeat * 50) + 1234;
    REQUIRE(static_cast<std::size_t>(kTarget) < kFrames);

    PluginRegistry::OpenResult opened = registry().open(url);
    REQUIRE(opened);
    REQUIRE(opened.decoder->seek(kTarget) == kTarget);

    const std::vector<float> after = drain(*opened.decoder, 44100 * 2);
    REQUIRE_FALSE(after.empty());

    const std::size_t common =
        std::min(after.size(), straight.samples.size() - (static_cast<std::size_t>(kTarget) * 2));
    REQUIRE(common > 44100);

    // Bit-identical, and that word is the point. Skipping a beat advances the
    // same phase accumulator by the same multiplication and shortens the same
    // remaining note lengths by the same count; the only thing it leaves out is
    // writing samples nobody asked for. Anything less than equality here means
    // the skip and the render have drifted apart -- which is the fault Cog has,
    // where a note still sounding across the seek point is simply gone.
    CHECK(std::equal(after.begin(), after.begin() + static_cast<std::ptrdiff_t>(common),
                     straight.samples.begin() + (static_cast<std::ptrdiff_t>(kTarget) * 2)));
}

TEST_CASE("seeking backwards replays from the top", "[organya]") {
    PluginRegistry::OpenResult opened = registry().open(writeTemp("tune.org", tuneBytes()));
    REQUIRE(opened);

    const std::vector<float> first = drain(*opened.decoder, 44100 * 2);
    REQUIRE_FALSE(first.empty());

    // Backwards is the case a forward-only sequencer cannot serve without
    // starting over, and starting over has to reach the same state it reached
    // the first time -- so the same frames come out.
    REQUIRE(opened.decoder->seek(0) == 0);
    const std::vector<float> again = drain(*opened.decoder, 44100 * 2);
    REQUIRE(again.size() >= first.size());
    CHECK(std::equal(first.begin(), first.end(), again.begin()));
}

TEST_CASE("percussion is synthesised, not silent", "[organya]") {
    // The drums are not in the file. They are synthesised at startup by a
    // miniature PixTone engine from six parameter sets compiled into the codec,
    // and if those went missing -- an install step that did not happen, a glob
    // that matched nothing -- every song would still play, just without any
    // percussion. Nothing else in this file would notice.
    //
    // So: the same song twice, once with a percussion track and once without,
    // and they have to differ.
    std::vector<Note> melody;
    for (int beat = 0; beat < 16; beat += 2) {
        melody.push_back(Note{beat, 44, 2, 220, 6});
    }
    std::vector<Note> drums;
    for (int beat = 0; beat < 16; beat += 4) {
        drums.push_back(Note{beat, 60, 1, 220, 6});
    }

    const Decoded dry =
        decode(writeTemp("dry.org", makeOrg(125, 0, 16, melody, {})), 44100);
    const Decoded wet =
        decode(writeTemp("wet.org", makeOrg(125, 0, 16, melody, drums)), 44100);
    REQUIRE_FALSE(dry.samples.empty());
    REQUIRE(wet.samples.size() == dry.samples.size());

    CHECK_FALSE(std::equal(dry.samples.begin(), dry.samples.end(), wet.samples.begin()));
}

TEST_CASE("real songs decode and have a length", "[organya][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_ORGANYA_CORPUS=<path> to run this");
    }
    const auto songs = findSongs(24);
    if (songs.empty()) {
        SKIP("corpus holds no .org files");
    }

    for (const fs::path& path : songs) {
        INFO(path.filename().string());
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(opened);

        const std::int64_t frames = opened.decoder->properties().totalFrames;
        CHECK(frames > 0);
        // The longest song in Cave Story's soundtrack is about four minutes;
        // with two loop passes and a fade, an hour would mean a loop point read
        // out of the wrong place in the header.
        CHECK(frames < static_cast<std::int64_t>(44100) * 3600);

        // Every field in the header is read as little-endian and the beat rate
        // is one of them; a byte-swapped read gives a plausible file that plays
        // at an implausible tempo.
        CHECK(opened.decoder->properties().format.sampleRate == 44100.0);
    }
}

TEST_CASE("real songs seek exactly too", "[organya][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_ORGANYA_CORPUS=<path> to run this");
    }
    const auto songs = findSongs(6);
    if (songs.empty()) {
        SKIP("corpus holds no .org files");
    }

    // The synthetic song has one melodic track and one percussion track. Real
    // songs use all sixteen, hold notes across beats, and change panning
    // mid-phrase -- which is what makes the state carried over a seek worth
    // more than one accumulator.
    constexpr std::size_t  kFrames = 44100 * 4;
    constexpr std::int64_t kTarget = 44100 * 2;

    bool tested = false;
    for (const fs::path& path : songs) {
        INFO(path.filename().string());
        const Url     url      = Url::fromLocalPath(path);
        const Decoded straight = decode(url, kFrames * 2);
        if (straight.samples.size() < kFrames * 2 || peak(straight.samples) == 0.0F) {
            continue;  // Too short, or one of the format's genuinely empty songs.
        }

        PluginRegistry::OpenResult opened = registry().open(url);
        REQUIRE(opened);
        REQUIRE(opened.decoder->seek(kTarget) == kTarget);

        const std::vector<float> after = drain(*opened.decoder, 44100 * 2);
        const std::size_t        common =
            std::min(after.size(),
                     straight.samples.size() - (static_cast<std::size_t>(kTarget) * 2));
        REQUIRE(common > 44100);
        CHECK(std::equal(
            after.begin(), after.begin() + static_cast<std::ptrdiff_t>(common),
            straight.samples.begin() + (static_cast<std::ptrdiff_t>(kTarget) * 2)));
        tested = true;
        break;
    }
    if (!tested) {
        SKIP("corpus holds no song long enough to seek four seconds into");
    }
}
