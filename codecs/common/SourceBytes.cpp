#include "SourceBytes.hpp"

namespace xpcog::codecs {

std::optional<std::vector<std::byte>> readAllBytes(ISource& source,
                                                   std::size_t limit) {
    std::vector<std::byte> out;
    std::byte              buffer[64U * 1024U];

    for (;;) {
        const std::int64_t got =
            source.read(buffer, static_cast<std::int64_t>(sizeof(buffer)));
        if (got < 0) {
            return std::nullopt;  // a real error, not the end
        }
        if (got == 0) {
            return out;
        }
        if (out.size() + static_cast<std::size_t>(got) > limit) {
            return std::nullopt;
        }
        out.insert(out.end(), buffer, buffer + got);
    }
}

}  // namespace xpcog::codecs
