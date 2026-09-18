#include "xpcog/core/audio/WaveformFile.hpp"

#include <bit>
#include <cstring>

namespace xpcog {
namespace {

constexpr std::size_t  kHeaderSize = 36;
constexpr std::uint8_t kVersion    = 1;
constexpr std::uint8_t kLayoutPeakRms = 1;
constexpr char         kMagic[4]   = {'X', 'P', 'W', 'F'};

// Written and read a byte at a time, so the file is the same on every machine
// and no struct padding or host order reaches the disk.

void putU32(std::byte* out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

void putU64(std::byte* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

[[nodiscard]] std::uint32_t getU32(const std::byte* in) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[i])) << (8 * i);
    }
    return v;
}

[[nodiscard]] std::uint64_t getU64(const std::byte* in) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[i])) << (8 * i);
    }
    return v;
}

}  // namespace

std::vector<std::byte> encodeWaveform(const WaveformSummary&    summary,
                                      const PluginCache::Stamp& stamp) {
    const std::uint32_t n = summary.bucketCount;
    std::vector<std::byte> out(waveformFileSize(n));
    std::byte* p = out.data();

    std::memcpy(p, kMagic, 4);
    p[4] = static_cast<std::byte>(kVersion);
    p[5] = static_cast<std::byte>(kLayoutPeakRms);
    p[6] = std::byte{0};
    p[7] = std::byte{0};
    putU32(p + 8, n);
    putU64(p + 12, std::bit_cast<std::uint64_t>(summary.duration));
    putU64(p + 20, static_cast<std::uint64_t>(stamp.modifiedSeconds));
    putU64(p + 28, static_cast<std::uint64_t>(stamp.sizeBytes));

    if (n > 0) {
        std::memcpy(p + kHeaderSize, summary.peak.data(), n);
        std::memcpy(p + kHeaderSize + n, summary.rms.data(), n);
    }
    return out;
}

std::optional<WaveformRecord> decodeWaveform(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::nullopt;
    }
    const std::byte* p = bytes.data();
    if (std::memcmp(p, kMagic, 4) != 0) {
        return std::nullopt;
    }
    if (std::to_integer<std::uint8_t>(p[4]) != kVersion ||
        std::to_integer<std::uint8_t>(p[5]) != kLayoutPeakRms) {
        return std::nullopt;
    }

    const std::uint32_t n = getU32(p + 8);
    if (n == 0 || bytes.size() != waveformFileSize(n)) {
        return std::nullopt;
    }

    WaveformRecord record;
    record.summary.bucketCount = n;
    record.summary.analysed    = n;
    record.summary.duration    = std::bit_cast<double>(getU64(p + 12));
    record.stamp.modifiedSeconds = static_cast<std::int64_t>(getU64(p + 20));
    record.stamp.sizeBytes       = static_cast<std::int64_t>(getU64(p + 28));

    const auto* peak = reinterpret_cast<const std::uint8_t*>(p + kHeaderSize);
    record.summary.peak.assign(peak, peak + n);
    record.summary.rms.assign(peak + n, peak + (2 * n));
    return record;
}

}  // namespace xpcog
