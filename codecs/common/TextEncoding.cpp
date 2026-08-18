#include "TextEncoding.hpp"

#include "xpcog/core/Utf8.hpp"

#include <utility>

namespace xpcog::codecs {

bool isValidUtf8(const std::string& text) { return xpcog::isValidUtf8(text); }

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
