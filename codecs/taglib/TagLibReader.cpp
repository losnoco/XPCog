// Static tag reading via TagLib. Replaces the family of Cog metadata readers
// (Plugins/VorbisMetadataReader, ID3, APEv2, ...) with one format-agnostic
// reader, which is what TagLib's PropertyMap interface exists to provide.
//
// Cog reaches for a per-format reader plugin and stops at the first that claims
// the extension. TagLib covers every format in M1's scope and most of M6's, so a
// single reader at default priority is the base layer; a format-specific reader
// registered above it supplements rather than replaces, because the registry
// merges in priority order.

#include "xpcog/core/PluginRegistry.hpp"

#include <fileref.h>
#include <tdebuglistener.h>
#include <tfile.h>
#include <tpropertymap.h>
#include <tvariant.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace xpcog {
namespace {

[[nodiscard]] std::string toUtf8(const TagLib::String& text) {
    return text.to8Bit(/*unicode=*/true);
}

/// Cog stores tag names lowercased (PlaylistEntry.m:608) and MetadataMap
/// normalises the same way; TagLib hands back uppercase. Converting here keeps
/// the tag names a user sees identical to Cog's.
[[nodiscard]] std::string lowercased(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return text;
}

/// TagLib writes parse complaints straight to stderr by default. Scanning a
/// music folder legitimately hands it JPEGs, text files and half-copied
/// downloads, so the default turns a normal scan into a wall of noise the user
/// can do nothing about. Failures are reported through the return value.
class SilentDebugListener : public TagLib::DebugListener {
public:
    void printMessage(const TagLib::String&) override {}
};

void silenceTagLibOnce() {
    static SilentDebugListener listener;
    static const bool          installed = [] {
        TagLib::setDebugListener(&listener);
        return true;
    }();
    static_cast<void>(installed);
}

MetadataMap readWithTagLib(const Url& url) {
    silenceTagLibOnce();

    MetadataMap tags;

    const auto path = url.localPath();
    if (!path) {
        // TagLib opens the container itself and seeks freely, so it needs a real
        // file. Archive and HTTP sources get a reader over ISource in M6; the
        // descriptor's shape does not change when that lands.
        return tags;
    }

    // A fragment names a cue track or subsong, not a different file: the tags
    // belong to the file itself.
    TagLib::FileRef file{TagLib::FileName{path->c_str()},
                         /*readAudioProperties=*/false};
    if (file.isNull() || file.file() == nullptr) {
        return tags;
    }

    for (const auto& [key, values] : file.properties()) {
        const std::string name = lowercased(toUtf8(key));
        for (const auto& value : values) {
            tags.add(name, toUtf8(value));
        }
    }

    // Cover art. Only the first picture is kept: Cog shows one image, and
    // carrying a booklet's worth of scans through the playlist would grow the
    // library by tens of megabytes for something nothing displays.
    for (const auto& picture : file.complexProperties("PICTURE")) {
        const auto data = picture.find("data");
        if (data == picture.end()) {
            continue;
        }
        const TagLib::ByteVector bytes = data->second.value<TagLib::ByteVector>();
        if (bytes.isEmpty()) {
            continue;
        }
        const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());
        tags.setBytes("albumart",
                      std::vector<std::byte>{begin, begin + bytes.size()});
        break;
    }

    return tags;
}

}  // namespace
}  // namespace xpcog

void xpcog_register_taglib(xpcog::PluginRegistry& r) {
    r.addMetadataReader({
        .name       = "TagLibReader",
        .priority   = xpcog::kDefaultPriority,
        .extensions = {},  // format-agnostic: offered for everything
        .read       = &xpcog::readWithTagLib,
        .available  = nullptr,
    });
}
