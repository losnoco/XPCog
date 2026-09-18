#include "xpcog/core/audio/WaveformCache.hpp"

#include "xpcog/core/Sha256.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/WaveformFile.hpp"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

[[nodiscard]] bool cacheable(const PluginCache::Stamp& stamp) noexcept {
    return stamp != PluginCache::Stamp{};
}

}  // namespace

WaveformCache::WaveformCache(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::string WaveformCache::keyFor(const Url& url, const PluginCache::Stamp& stamp) {
    // NUL-separated so a URL ending in digits cannot run into the stamp.
    std::string text = url.toString();
    text += '\0';
    text += std::to_string(stamp.modifiedSeconds);
    text += '\0';
    text += std::to_string(stamp.sizeBytes);

    const std::string hex = sha256Hex(std::as_bytes(std::span{text}));
    return hex.substr(0, 32) + ".xpwf";
}

std::optional<WaveformSummary> WaveformCache::load(const Url& url) const {
    const PluginCache::Stamp stamp = PluginCache::stampFor(url);
    if (!cacheable(stamp)) {
        return std::nullopt;
    }

    std::ifstream file(directory_ / keyFor(url, stamp), std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

    auto record = decodeWaveform(std::as_bytes(std::span{bytes}));
    if (!record || record->stamp != stamp) {
        // A file under this name with another stamp inside is not an entry for
        // this track, whatever the name says.
        return std::nullopt;
    }
    return std::move(record->summary);
}

bool WaveformCache::store(const Url& url, const WaveformSummary& summary) const {
    if (!summary.complete()) {
        return false;
    }
    const PluginCache::Stamp stamp = PluginCache::stampFor(url);
    if (!cacheable(stamp)) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);

    const std::filesystem::path target = directory_ / keyFor(url, stamp);
    std::filesystem::path       temporary = target;
    temporary += ".tmp";

    const std::vector<std::byte> bytes = encodeWaveform(summary, stamp);
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            return false;
        }
    }

    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        // Windows will not rename over an existing file on every filesystem;
        // remove and retry rather than lose the write.
        std::filesystem::remove(target, ec);
        std::filesystem::rename(temporary, target, ec);
    }
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

}  // namespace xpcog
