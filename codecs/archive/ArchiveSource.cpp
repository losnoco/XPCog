// Reading a file out of an archive.
//
// Port of Cog Plugins/ArchiveSource (ArchiveSource, ArchiveContainer), on
// libarchive rather than File_Extractor. The third and last ISource: after this,
// the seam has a local file, a network stream and a compressed member behind it,
// which between them cover every shape the interface was written for.
//
// The member is decompressed whole into memory at open(). That is what Cog does
// too (fex_data hands back the entire block), and it is not laziness: a member
// of a solid archive cannot be seeked without decompressing everything before
// it, so "stream it" would mean re-reading from the start on every backward
// seek. Music files are small enough that holding one is cheaper than that, and
// decoders seek constantly.
//
// This file is the *several tracks* half of archive handling. The other half --
// a `.itz` or `.mdz`, which is one module in a wrapper and is not meant to look
// like an archive at all -- is CompressedFileSource, next door.
//
// Cog's SandboxBroker calls around the archive are absent, as everywhere else --
// that is the macOS App Sandbox, and the seam for it is platform::IFileAccess.

#include "ArchiveReader.hpp"
#include "CompressedFileSource.hpp"
#include "UnpackUrl.hpp"

#include "common/BlobSource.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <archive_entry.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

/// Cog's list. `rsn` is a RAR of SPC files and `vgm7z` a 7z of VGMs -- renamed
/// so a music library can tell them apart at a glance, and identified by content
/// rather than by name, so nothing here has to care.
constexpr std::string_view kArchiveExtensions[] = {"zip", "rar",   "7z",
                                                   "rsn", "vgm7z", "gz"};

constexpr std::string_view kArchiveMimeTypes[] = {
    "application/zip", "application/x-gzip", "application/x-rar-compressed",
    "application/x-7z-compressed"};

class ArchiveSource final : public codecs::BlobSource {
public:
    bool open(const Url& url) override {
        close();

        const auto target = codecs::parseUnpackUrl(url);
        if (!target) {
            return false;
        }

        const codecs::ArchivePtr handle =
            codecs::openArchiveFile(pathFromUtf8(target->archive));
        if (!handle) {
            return false;
        }

        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(handle.get(), &entry) == ARCHIVE_OK) {
            if (codecs::entryName(entry) != target->member) {
                archive_read_data_skip(handle.get());
                continue;
            }

            auto member = codecs::readEntry(handle.get(), archive_entry_size(entry));
            if (!member) {
                return false;
            }
            setBlob(std::move(*member));
            url_ = url;
            return true;
        }

        // Named a member the archive does not hold: a stale playlist entry, or
        // an archive that has been rebuilt since it was scanned.
        return false;
    }

    [[nodiscard]] const Url& url() const override { return url_; }

private:
    Url url_;
};

/// One URL per member the build can actually play. Cog filters the same way,
/// against the registry's extension list rather than a list of its own -- so an
/// archive stops offering formats when the codec for them is switched off, and
/// starts offering new ones the moment a decoder claims them.
std::vector<Url> expandArchive(const Url& url, ISource& /*source*/,
                               const PluginRegistry& registry) {
    const auto path = url.localPath();
    if (!path) {
        // The archive path is baked into every member URL below, so it has to be
        // a real filesystem path. An archive over HTTP would need libarchive
        // driven through its read callbacks instead; Cog declines this the same
        // way, with an isFileURL check.
        return {url};
    }

    const codecs::ArchivePtr handle = codecs::openArchiveFile(*path);
    if (!handle) {
        return {url};
    }

    std::vector<Url> members;
    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(handle.get(), &entry) == ARCHIVE_OK) {
        if (archive_entry_filetype(entry) != AE_IFREG) {
            continue;  // directories and links are not tracks
        }

        const std::string name = codecs::entryName(entry);
        if (name.empty() || codecs::isArchiveJunk(name)) {
            continue;
        }

        const Url member = codecs::makeUnpackUrl(*path, name);
        if (member.empty() || !registry.isPlayableExtension(member.extension())) {
            continue;
        }
        members.push_back(member);
    }

    // An archive holding nothing playable stays itself rather than becoming an
    // empty expansion, which callers apply uniformly and would read as "this
    // file vanished".
    return members.empty() ? std::vector<Url>{url} : members;
}

constexpr std::string_view kSchemes[] = {"unpack"};

}  // namespace
}  // namespace xpcog

void xpcog_register_archive(xpcog::PluginRegistry& r) {
    r.addSource({
        .name    = "ArchiveSource",
        .schemes = xpcog::kSchemes,
        .create  = []() -> xpcog::SourcePtr {
            return std::make_unique<xpcog::ArchiveSource>();
        },
    });

    // Not a source of its own: it wraps whichever source the URL's scheme picks,
    // so a `.itz` unwraps the same whether it is on disk, inside another
    // archive, or coming down an HTTP connection.
    r.addSourceWrapper({
        .name       = "CompressedFileSource",
        .extensions = xpcog::codecs::compressedModuleExtensions(),
        .wrap       = &xpcog::codecs::makeCompressedFileSource,
    });

    r.addContainer({
        .name       = "ArchiveContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kArchiveExtensions,
        .mimeTypes  = xpcog::kArchiveMimeTypes,
        .expand     = &xpcog::expandArchive,
    });
}
