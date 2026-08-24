#include "xpcog/platform/PropertyListFile.hpp"

#include <cstdio>
#include <string>
#include <string_view>
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

    // Everything else has to look like XML before it is handed back. "Not
    // bplist00" is not the same as "a property list": Cog's application support
    // directory sits next to a SQLite database, and returning its bytes as XML
    // would push the mistake down to a parser that can only answer the same
    // nullopt one step later, after the caller has been told there was something
    // to parse.
    std::string_view start{text};
    if (start.starts_with("\xEF\xBB\xBF")) {  // A BOM, if some editor left one.
        start.remove_prefix(3);
    }
    while (!start.empty() && (start.front() == ' ' || start.front() == '\t' ||
                              start.front() == '\r' || start.front() == '\n')) {
        start.remove_prefix(1);
    }
    if (!start.starts_with('<')) {
        return std::nullopt;
    }
    // XML of some sort. Whether it is a *property* list the caller parses and
    // finds out; that much is the same judgement on every platform.
    return text;
}

}  // namespace xpcog::platform
