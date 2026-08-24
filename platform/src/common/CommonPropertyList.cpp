#include "xpcog/platform/PropertyListFile.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace xpcog::platform {

std::optional<std::string> propertyListToXml(const std::filesystem::path& path) {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return std::nullopt;
    }
    std::string text;
    char        buffer[8192];
    while (const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file)) {
        text.append(buffer, got);
    }
    std::fclose(file);

    // Apple's binary format, which only CoreFoundation reads. Answering nullopt
    // rather than attempting it is the honest result: the alternative is a
    // second implementation of a format whose only reader on this project's
    // path belongs to a program that does not run here.
    if (text.starts_with("bplist00")) {
        return std::nullopt;
    }
    if (text.empty()) {
        return std::nullopt;
    }
    // Already XML, or at least not binary. The caller parses it and finds out.
    return text;
}

}  // namespace xpcog::platform
