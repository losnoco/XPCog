#include "xpcog/core/Utf8.hpp"

#include <cstddef>

namespace xpcog {

bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);

        std::size_t continuation = 0;
        if (byte < 0x80) {
            continuation = 0;
        } else if ((byte & 0xE0) == 0xC0) {
            continuation = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            continuation = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            continuation = 3;
        } else {
            return false;
        }

        if (i + continuation >= text.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= continuation; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += continuation + 1;
    }
    return true;
}

}  // namespace xpcog
