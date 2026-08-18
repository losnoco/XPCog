#include "TextEncoding.hpp"

#include <utility>

namespace xpcog::codecs {

bool isValidUtf8(const std::string& text) {
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

std::string latin1ToUtf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            out.push_back(static_cast<char>(byte));
        } else {
            out.push_back(static_cast<char>(0xC0 | (byte >> 6)));
            out.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
        }
    }
    return out;
}

std::string toUtf8(std::string text) {
    return isValidUtf8(text) ? std::move(text) : latin1ToUtf8(text);
}

}  // namespace xpcog::codecs
