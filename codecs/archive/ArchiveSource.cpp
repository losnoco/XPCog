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
// Cog's SandboxBroker calls around the archive are absent, as everywhere else --
// that is the macOS App Sandbox, and the seam for it is platform::IFileAccess.

#include "UnpackUrl.hpp"

#include "common/TextEncoding.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
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

/// libarchive's reader, closed and freed however the scope ends.
using ArchivePtr = std::unique_ptr<struct archive, decltype(&archive_read_free)>;

[[nodiscard]] ArchivePtr openArchive(const std::string& path) {
    ArchivePtr handle{archive_read_new(), &archive_read_free};
    if (!handle) {
        return {nullptr, &archive_read_free};
    }

    // Every format and every compression filter the build supports: the
    // extension is a hint from the file's name, and rsn and vgm7z are proof
    // that the name can be anything at all.
    archive_read_support_filter_all(handle.get());
    archive_read_support_format_all(handle.get());

    constexpr std::size_t kBlockSize = 64U * 1024U;
    if (archive_read_open_filename(handle.get(), path.c_str(), kBlockSize) !=
        ARCHIVE_OK) {
        return {nullptr, &archive_read_free};
    }
    return handle;
}

/// The entry's name, preferring libarchive's UTF-8 accessor and falling back to
/// the raw bytes put through the same guess every other untagged text here gets.
[[nodiscard]] std::string entryName(struct archive_entry* entry) {
    if (const char* utf8 = archive_entry_pathname_utf8(entry); utf8 != nullptr) {
        return utf8;
    }
    if (const char* raw = archive_entry_pathname(entry); raw != nullptr) {
        return codecs::toUtf8(raw);
    }
    return {};
}

class ArchiveSource final : public ISource {
public:
    bool open(const Url& url) override {
        close();

        const auto target = codecs::parseUnpackUrl(url);
        if (!target) {
            return false;
        }

        ArchivePtr handle = openArchive(target->archive);
        if (!handle) {
            return false;
        }

        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(handle.get(), &entry) == ARCHIVE_OK) {
            if (entryName(entry) != target->member) {
                archive_read_data_skip(handle.get());
                continue;
            }

            // archive_entry_size is unreliable for some formats, so the read
            // loop below is the authority and the size is only a hint for how
            // much to reserve.
            if (const la_int64_t hint = archive_entry_size(entry); hint > 0) {
                data_.reserve(static_cast<std::size_t>(hint));
            }

            std::byte chunk[64U * 1024U];
            for (;;) {
                const la_ssize_t got =
                    archive_read_data(handle.get(), chunk, sizeof(chunk));
                if (got == 0) {
                    break;  // end of the member
                }
                if (got < 0) {
                    data_.clear();
                    return false;
                }
                data_.insert(data_.end(), chunk, chunk + got);
            }

            url_    = url;
            offset_ = 0;
            return true;
        }

        // Named a member the archive does not hold: a stale playlist entry, or
        // an archive that has been rebuilt since it was scanned.
        return false;
    }

    [[nodiscard]] bool seekable() const override { return true; }

    bool seek(std::int64_t position, int whence) override {
        const auto size = static_cast<std::int64_t>(data_.size());
        switch (whence) {
            case SEEK_CUR: position += offset_; break;
            case SEEK_END: position += size; break;
            default: break;
        }
        if (position < 0) {
            return false;
        }
        offset_ = position;
        return offset_ <= size;
    }

    [[nodiscard]] std::int64_t tell() const override { return offset_; }

    std::int64_t read(void* buffer, std::int64_t bytes) override {
        const auto size = static_cast<std::int64_t>(data_.size());
        if (bytes <= 0 || offset_ >= size) {
            return 0;
        }
        const std::int64_t take = std::min(bytes, size - offset_);
        std::memcpy(buffer, data_.data() + offset_, static_cast<std::size_t>(take));
        offset_ += take;
        return take;
    }

    void close() override {
        data_.clear();
        data_.shrink_to_fit();
        offset_ = 0;
    }

    [[nodiscard]] const Url& url() const override { return url_; }

private:
    Url                    url_;
    std::vector<std::byte> data_;
    std::int64_t           offset_ = 0;
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

    ArchivePtr handle = openArchive(path->string());
    if (!handle) {
        return {url};
    }

    std::vector<Url> members;
    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(handle.get(), &entry) == ARCHIVE_OK) {
        if (archive_entry_filetype(entry) != AE_IFREG) {
            continue;  // directories and links are not tracks
        }

        const std::string name = entryName(entry);
        if (name.empty()) {
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

    r.addContainer({
        .name       = "ArchiveContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kArchiveExtensions,
        .mimeTypes  = xpcog::kArchiveMimeTypes,
        .expand     = &xpcog::expandArchive,
    });
}
