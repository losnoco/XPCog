#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <numeric>
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
