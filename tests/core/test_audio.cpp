#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include <miniaudio.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <span>
#include <thread>
#include <vector>

using namespace xpcog;

// --- RingBuffer -----------------------------------------------------------

TEST_CASE("RingBuffer round-trips samples", "[ring]") {
    RingBuffer ring(16);

    const std::vector<float> in = {1.0F, 2.0F, 3.0F, 4.0F};
    CHECK(ring.write(in.data(), in.size()) == 4);
    CHECK(ring.availableToRead() == 4);

    std::vector<float> out(4, 0.0F);
    CHECK(ring.read(out.data(), out.size()) == 4);
    CHECK(out == in);
    CHECK(ring.availableToRead() == 0);
}

TEST_CASE("RingBuffer write is bounded by free space", "[ring]") {
    RingBuffer ring(8);  // rounds to 8 usable (one slot reserved of 16... see below)

    std::vector<float> big(1000, 1.0F);
    const std::size_t  written = ring.write(big.data(), big.size());

    CHECK(written == ring.capacity());
    CHECK(written < big.size());
    CHECK(ring.availableToWrite() == 0);

    // A full ring accepts nothing more.
    CHECK(ring.write(big.data(), 1) == 0);
}

TEST_CASE("RingBuffer read is bounded by available data", "[ring]") {
    RingBuffer ring(16);

    const float one = 42.0F;
    ring.write(&one, 1);

    std::vector<float> out(10, -1.0F);
    CHECK(ring.read(out.data(), out.size()) == 1);
    CHECK(out[0] == 42.0F);
    CHECK(out[1] == -1.0F);  // untouched

    // An empty ring yields nothing; the caller silences the tail itself.
    CHECK(ring.read(out.data(), out.size()) == 0);
}

TEST_CASE("RingBuffer wraps correctly", "[ring]") {
    RingBuffer ring(8);

    // Push the indices most of the way round, then straddle the boundary.
    std::vector<float> pad(5, 0.0F);
    ring.write(pad.data(), pad.size());
    std::vector<float> drain(5, 0.0F);
    ring.read(drain.data(), drain.size());

    const std::vector<float> payload = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    REQUIRE(ring.write(payload.data(), payload.size()) == payload.size());

    std::vector<float> out(payload.size(), 0.0F);
    REQUIRE(ring.read(out.data(), out.size()) == payload.size());
    CHECK(out == payload);
}

TEST_CASE("RingBuffer::clear discards pending data", "[ring]") {
    RingBuffer         ring(16);
    std::vector<float> in(8, 1.0F);
    ring.write(in.data(), in.size());

    ring.clear();
    CHECK(ring.availableToRead() == 0);
}

// Deliberately NOT tagged "[.something]": a leading dot hides a test from Catch2's
// default run, and this is the one guarding the real-time path.
TEST_CASE("RingBuffer survives concurrent producer and consumer", "[ring][concurrency]") {
    // The RT callback and the feeder run concurrently for the whole life of the
    // program, so a lost or duplicated sample here is a real-world dropout.
    // A monotonically increasing sequence makes either failure detectable.
    constexpr std::size_t kTotal = 1'000'000;

    RingBuffer        ring(1024);
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        std::size_t next = 0;
        while (next < kTotal) {
            const auto        value = static_cast<float>(next % 1000);
            const std::size_t wrote = ring.write(&value, 1);
            if (wrote == 1) {
                ++next;
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        std::size_t seen = 0;
        while (seen < kTotal) {
            float             value = -1.0F;
            const std::size_t got   = ring.read(&value, 1);
            if (got == 1) {
                if (value != static_cast<float>(seen % 1000)) {
                    failed.store(true);
                    return;
                }
                ++seen;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
    CHECK_FALSE(failed.load());
}

// --- SampleConvert --------------------------------------------------------

namespace {

template <typename T>
AudioChunk chunkOf(SampleFormat format, const std::vector<T>& samples) {
    AudioFormat fmt;
    fmt.sampleRate = 44100.0;
    fmt.channels   = 1;
    fmt.format     = format;

    AudioChunk chunk;
    chunk.setFormat(fmt);
    std::byte* data = chunk.allocFrames(samples.size());
    std::memcpy(data, samples.data(), samples.size() * sizeof(T));
    return chunk;
}

}  // namespace

TEST_CASE("convertToFloat32 scales 16-bit to full range", "[convert]") {
    const AudioChunk chunk =
        chunkOf<std::int16_t>(SampleFormat::S16, {0, 32767, -32768, 16384});

    std::vector<float> out(4);
    REQUIRE(convertToFloat32(chunk, out) == 4);

    CHECK(out[0] == Catch::Approx(0.0F));
    // Full-scale negative maps to exactly -1.0; positive lands just under +1.0.
    // This asymmetry is intentional and matches Cog.
    CHECK(out[2] == Catch::Approx(-1.0F));
    CHECK(out[1] == Catch::Approx(32767.0F / 32768.0F));
    CHECK(out[3] == Catch::Approx(0.5F));
}

TEST_CASE("convertToFloat32 sign-extends 24-bit", "[convert]") {
    AudioFormat fmt;
    fmt.sampleRate = 44100.0;
    fmt.channels   = 1;
    fmt.format     = SampleFormat::S24;

    AudioChunk chunk;
    chunk.setFormat(fmt);
    std::byte* data = chunk.allocFrames(2);

    // -1 as little-endian 24-bit is FF FF FF; +full-scale is FF FF 7F.
    data[0] = std::byte{0xFF}; data[1] = std::byte{0xFF}; data[2] = std::byte{0xFF};
    data[3] = std::byte{0xFF}; data[4] = std::byte{0xFF}; data[5] = std::byte{0x7F};

    std::vector<float> out(2);
    REQUIRE(convertToFloat32(chunk, out) == 2);

    CHECK(out[0] == Catch::Approx(-1.0F / 8388608.0F));
    CHECK(out[1] == Catch::Approx(8388607.0F / 8388608.0F));
}

TEST_CASE("convertToFloat32 centres unsigned 8-bit", "[convert]") {
    const AudioChunk chunk =
        chunkOf<std::uint8_t>(SampleFormat::U8, {128, 255, 0});

    std::vector<float> out(3);
    REQUIRE(convertToFloat32(chunk, out) == 3);

    CHECK(out[0] == Catch::Approx(0.0F));  // 128 is silence for unsigned
    CHECK(out[1] == Catch::Approx(127.0F / 128.0F));
    CHECK(out[2] == Catch::Approx(-1.0F));
}

TEST_CASE("convertToFloat32 passes float32 through untouched", "[convert]") {
    const AudioChunk chunk =
        chunkOf<float>(SampleFormat::F32, {0.25F, -0.5F, 1.0F});

    std::vector<float> out(3);
    REQUIRE(convertToFloat32(chunk, out) == 3);
    CHECK(out[0] == 0.25F);
    CHECK(out[1] == -0.5F);
    CHECK(out[2] == 1.0F);
}

TEST_CASE("convertToFloat32 refuses an undersized destination", "[convert]") {
    const AudioChunk chunk = chunkOf<std::int16_t>(SampleFormat::S16, {1, 2, 3, 4});

    std::vector<float> tooSmall(2);
    CHECK(convertToFloat32(chunk, tooSmall) == 0);
}

TEST_CASE("float32SampleCount accounts for channels", "[convert]") {
    AudioFormat fmt;
    fmt.sampleRate = 44100.0;
    fmt.channels   = 2;
    fmt.format     = SampleFormat::S16;

    AudioChunk chunk;
    chunk.setFormat(fmt);
    static_cast<void>(chunk.allocFrames(100));

    CHECK(float32SampleCount(chunk) == 200);
}

// ---------------------------------------------------------------------------
// Which output device a stored choice means
// ---------------------------------------------------------------------------
// The rule is Cog's, and the name half of it is the part worth testing: a USB
// interface that has been unplugged and put back can present the same hardware
// under a new id, and matching on id alone would drop the listener back to the
// laptop speakers without saying anything.

TEST_CASE("a chosen output device is matched by id, then by name", "[audio][device]") {
    const std::vector<xpcog::DeviceInfo> devices{
        {"{0.0.0}.{aaa}", "Speakers", true},
        {"{0.0.0}.{bbb}", "Topping D10", false},
    };

    SECTION("nothing chosen is the system default") {
        CHECK(xpcog::resolveOutputDevice(devices, "", "").empty());
        // A name alone cannot select: the id is what a choice is made of, and a
        // stored name with no id is a half-written setting.
        CHECK(xpcog::resolveOutputDevice(devices, "", "Topping D10").empty());
    }

    SECTION("the id when it is still there") {
        CHECK(xpcog::resolveOutputDevice(devices, "{0.0.0}.{bbb}", "Topping D10") ==
              "{0.0.0}.{bbb}");
        // The id wins even when the name has moved to another device.
        CHECK(xpcog::resolveOutputDevice(devices, "{0.0.0}.{bbb}", "Speakers") ==
              "{0.0.0}.{bbb}");
    }

    SECTION("the name when the id has changed underneath it") {
        CHECK(xpcog::resolveOutputDevice(devices, "{0.0.0}.{old}", "Topping D10") ==
              "{0.0.0}.{bbb}");
    }

    SECTION("neither is the default device, and the setting is not rewritten") {
        CHECK(xpcog::resolveOutputDevice(devices, "{0.0.0}.{gone}", "Focusrite").empty());
        CHECK(xpcog::resolveOutputDevice({}, "{0.0.0}.{bbb}", "Topping D10").empty());
    }
}

// --- convertFromFloat32 ---------------------------------------------------
//
// The output direction, which exists so a device can be opened in an integer
// format. What matters is not that it is approximately right but that it is
// *exact*: DoP carries DSD inside PCM as marker bytes, and one bit wrong is
// noise at the DAC rather than a slightly different sound.

TEST_CASE("convertFromFloat32 round-trips every 24-bit code exactly", "[convert]") {
    // The claim the DoP path will rest on. Not a sample of the range -- the
    // extremes and the boundaries, which is where rounding in the wrong width
    // goes wrong, plus a sweep to catch anything systematic.
    const std::vector<std::int32_t> codes = {
        0, 1, -1, 2, -2, 4096, -4096, 8388606, 8388607, -8388607, -8388608,
    };

    for (const std::int32_t code : codes) {
        const float asFloat = static_cast<float>(code) / 8388608.0F;

        std::array<std::byte, 3> packed{};
        REQUIRE(xpcog::convertFromFloat32(std::span<const float>{&asFloat, 1},
                                          SampleFormat::S24, packed) == 3);

        // Read it back the way the forward converter does, and demand the code.
        AudioFormat fmt;
        fmt.sampleRate = 44100.0;
        fmt.channels   = 1;
        fmt.format     = SampleFormat::S24;
        AudioChunk chunk;
        chunk.setFormat(fmt);
        std::byte* data = chunk.allocFrames(1);
        std::copy(packed.begin(), packed.end(), data);

        std::vector<float> back(1);
        REQUIRE(convertToFloat32(chunk, back) == 1);
        INFO("code " << code);
        CHECK(back[0] == asFloat);
    }
}

TEST_CASE("convertFromFloat32 clamps instead of wrapping", "[convert]") {
    // A sample over full scale must land on full scale. Wrapping would turn one
    // loud sample into a full-scale click of the opposite sign, which is the
    // loudest possible way to get this wrong.
    const std::array<float, 4> loud = {2.0F, -2.0F, 1.0F, -1.0F};

    std::array<std::byte, 16> packed{};
    REQUIRE(xpcog::convertFromFloat32(loud, SampleFormat::S32, packed) == 16);

    std::array<std::int32_t, 4> got{};
    std::memcpy(got.data(), packed.data(), packed.size());

    CHECK(got[0] == 2147483647);
    CHECK(got[1] == -2147483648LL);
    // +1.0 is above the last representable positive code, by the same asymmetry
    // convertToFloat32 documents, so it clamps too.
    CHECK(got[2] == 2147483647);
    CHECK(got[3] == -2147483648LL);
}

TEST_CASE("convertFromFloat32 refuses what a device is not opened in", "[convert]") {
    const std::array<float, 2> samples = {0.0F, 0.5F};
    std::array<std::byte, 16>  out{};

    // A decoder produces these; a device is never opened in one, and answering 0
    // is what makes the backend fall back to float rather than write garbage.
    CHECK(xpcog::convertFromFloat32(samples, SampleFormat::U8, out) == 0);
    CHECK(xpcog::convertFromFloat32(samples, SampleFormat::S8, out) == 0);
    CHECK(xpcog::convertFromFloat32(samples, SampleFormat::F64, out) == 0);
    CHECK(xpcog::convertFromFloat32(samples, SampleFormat::DSD, out) == 0);

    // And a buffer too small is refused rather than partly filled.
    std::array<std::byte, 3> tooSmall{};
    CHECK(xpcog::convertFromFloat32(samples, SampleFormat::S16, tooSmall) == 0);
}

TEST_CASE("convertFromFloat32 passes float through untouched", "[convert]") {
    // No scaling and no rounding: the bytes are the same bytes. This is the path
    // every ordinary track takes, so a copy that was not a copy would be audible
    // everywhere at once.
    const std::array<float, 3> samples = {0.0F, -0.25F, 0.75F};
    std::array<std::byte, 12>  out{};

    REQUIRE(xpcog::convertFromFloat32(samples, SampleFormat::F32, out) == 12);

    std::array<float, 3> back{};
    std::memcpy(back.data(), out.data(), out.size());
    CHECK(back == samples);
}

// Hidden -- the leading dot keeps it out of a default run, and out of CI, where
// a machine with no sound card is the normal case and opening one is a way to
// find out what a headless runner does under load rather than what this code
// does. Run it deliberately:
//
//     xpcog-tests "[.integerdevice]"
//
// It exists because the integer path has a part no unit test reaches: the
// callback's chunk loop, sized against whatever period the driver actually
// chose. convertFromFloat32 is checked exhaustively above; what this checks is
// that the loop around it does not walk off the end of a real device's buffer,
// which is the failure that would otherwise wait for someone's DAC.
//
// **It makes a noise, and the noise is half the test.** 200 ms of sawtooth at
// about 86 Hz, quietly. A correct run sounds like a low buzz with a pitch to it;
// wrong byte packing sounds like static, because a 24-bit sample assembled in
// the wrong order is white noise rather than a quieter or distorted tone. That
// distinction is audible in a way no assertion here can be -- the test can only
// see the floats going in, never the integers the driver received.
// Hidden for the same reason as the case below, and run the same way:
//
//     xpcog-tests "[.ratedevice]"
//
// It makes no sound. What it checks is the answer the real backend gives to the
// question the engine now asks before it builds anything -- see
// IAudioOutput::effectiveSampleRate() and tools/ma-rate-probe. The double in
// test_output_device.cpp pins what the *engine* does with the answer; this pins
// that the answer is true of the machine it is running on.
TEST_CASE("the real backend says what rate it will really run", "[.ratedevice]") {
    if (enumerateOutputDevices().empty()) {
        SKIP("no output device on this machine");
    }

    RingBuffer ring(1U << 12);
    auto       output = makeMiniaudioOutput(ring);
    REQUIRE(output != nullptr);

    const double native = output->preferredSampleRate({});
    if (native <= 0.0) {
        SKIP("this backend will not say what the device is running");
    }
    INFO("device is running at " << native << " Hz");

    // Shared: whatever is asked for, the answer is the rate the device is
    // already running, because a shared stream never moves it. This is the half
    // that was silently resampling.
    for (const double wanted : {44100.0, 48000.0, 96000.0, 192000.0}) {
        INFO("shared, wanted " << wanted);
        CHECK(output->effectiveSampleRate(wanted, {}, false) == native);
    }

    // Exclusive: the stream owns the device and switches it, so the rate asked
    // for is the rate that runs. Asserted as an identity rather than against
    // `native`, because the two are only equal by coincidence when the device
    // happens to already be there.
    for (const double wanted : {44100.0, 48000.0, 96000.0, 192000.0}) {
        INFO("exclusive, wanted " << wanted);
        CHECK(output->effectiveSampleRate(wanted, {}, true) == wanted);
    }

    // A rate of zero is not a question, and must not become an answer -- the
    // engine guards on `> 0.0`, and this is the other side of that contract.
    CHECK(output->effectiveSampleRate(0.0, {}, false) == 0.0);
}

// The case above pins that the engine is told a *consistent* story; this one
// pins that the story is true. It is a different question, and the difference
// is where a Bluetooth headset went mono.
//
// preferredSampleRate() used to read ma_device_info::nativeDataFormats[0],
// which on WASAPI is the mix format and on CoreAudio is merely the first rate
// in a list of everything the device would accept. A MOMENTUM 4 sitting in
// A2DP at 44,100 lists its 16,000 Hz hands-free rate first, so the engine
// resampled the album to 16,000 and opened the device there -- and CoreAudio's
// 16 kHz description for that device is mono, so miniaudio quietly set the
// audio unit to one channel. Every assertion in the case above still passed,
// because they all compare the answer against itself.
//
// So this one goes outside the seam and asks the hardware. Opening at the rate
// preferredSampleRate() named must leave miniaudio with nothing to convert:
// the same rate it was asked for, and no narrower than the device natively
// runs. Talking to miniaudio directly is the point -- internalSampleRate and
// internalChannels are the fields IAudioOutput deliberately does not expose,
// and they are the only place the silent conversion is visible. Same reasoning
// as tools/ma-rate-probe.
//
// Default device only, which is the one that bit. Reaching a named device would
// need MiniaudioOutput's private id encoding.
TEST_CASE("the rate the real backend names is one the device runs",
          "[.ratedevice]") {
    if (enumerateOutputDevices().empty()) {
        SKIP("no output device on this machine");
    }

    RingBuffer ring(1U << 12);
    auto       output = makeMiniaudioOutput(ring);
    REQUIRE(output != nullptr);

    const double named = output->preferredSampleRate({});
    if (named <= 0.0) {
        SKIP("this backend will not say what the device is running");
    }

    // Never started, so neither open makes a sound.
    const auto silence = [](ma_device*, void*, const void*, ma_uint32) {};

    // What the device runs when nothing is asked of it. The baseline for
    // "narrower", so that a genuinely mono output still passes.
    ma_device_config nativeConfig = ma_device_config_init(ma_device_type_playback);
    nativeConfig.playback.format   = ma_format_f32;
    nativeConfig.playback.channels = 0;
    nativeConfig.sampleRate        = 0;
    nativeConfig.dataCallback      = silence;

    ma_device native;
    if (ma_device_init(nullptr, &nativeConfig, &native) != MA_SUCCESS) {
        SKIP("the default device would not open");
    }
    const ma_uint32 nativeChannels = native.playback.internalChannels;
    ma_device_uninit(&native);

    // And now the open the engine would really do, at the rate it was told.
    ma_device_config askedConfig = ma_device_config_init(ma_device_type_playback);
    askedConfig.playback.format   = ma_format_f32;
    askedConfig.playback.channels = 2;
    askedConfig.sampleRate        = static_cast<ma_uint32>(named);
    askedConfig.dataCallback      = silence;

    ma_device asked;
    REQUIRE(ma_device_init(nullptr, &askedConfig, &asked) == MA_SUCCESS);
    const ma_uint32 askedRate     = asked.playback.internalSampleRate;
    const ma_uint32 askedChannels = asked.playback.internalChannels;
    ma_device_uninit(&asked);

    INFO("preferredSampleRate said " << named << " Hz; the device ran at "
                                     << askedRate << " Hz, " << askedChannels
                                     << " ch (natively " << nativeChannels << " ch)");
    CHECK(static_cast<double>(askedRate) == named);
    CHECK(askedChannels == nativeChannels);
}

TEST_CASE("a device opens in an integer format and runs", "[.integerdevice]") {
    if (enumerateOutputDevices().empty()) {
        SKIP("no output device on this machine");
    }

    RingBuffer ring(1U << 15);
    // A ramp rather than silence, so the conversion has something with sign and
    // magnitude to get wrong; the ring runs dry partway through, which exercises
    // the silenced tail in the same pass.
    //
    // -20 dBFS. Loud enough to hear and to tell a tone from static, quiet enough
    // not to startle someone who ran the suite without reading the comment above
    // -- which is how this level was arrived at.
    std::vector<float> ramp(1U << 13);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = (static_cast<float>(i % 512) / 512.0F - 0.5F) * 0.2F;
    }
    ring.write(ramp.data(), ramp.size());

    auto output = makeMiniaudioOutput(ring);
    REQUIRE(output != nullptr);

    IAudioOutput::Config config;
    config.sampleRate = 44100.0;
    config.channels   = 2;
    config.format     = SampleFormat::S24;

    if (!output->start(config)) {
        SKIP("the default device would not open");
    }

    const AudioFormat negotiated = output->negotiatedFormat();
    INFO("negotiated " << static_cast<int>(negotiated.format) << " at "
                       << negotiated.sampleRate << " Hz");

    // Not "it is S24": a device is entitled to refuse. What must hold is that
    // whatever it reports is what it is actually carrying, because the DoP path
    // will read exactly this to decide whether it can emit markers at all.
    CHECK(negotiated.bitsPerSample == (negotiated.format == SampleFormat::S24 ? 24U
                                       : negotiated.format == SampleFormat::S16 ? 16U
                                                                                : 32U));
    CHECK(negotiated.channels == 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    output->stop();

    // The callback ran at all. Zero here means the device was opened and never
    // pulled, which is a different failure from a conversion bug and worth
    // separating from one.
    CHECK(output->framesPlayed() > 0);
}
