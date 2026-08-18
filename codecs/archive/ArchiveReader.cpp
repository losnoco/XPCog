#include "ArchiveReader.hpp"

#include "common/TextEncoding.hpp"

#include <archive_entry.h>

namespace xpcog::codecs {
namespace {

constexpr std::size_t kBlockSize = 64U * 1024U;

[[nodiscard]] ArchivePtr newReader() {
    ArchivePtr handle{archive_read_new(), &archive_read_free};
    if (handle) {
        archive_read_support_filter_all(handle.get());
        archive_read_support_format_all(handle.get());
    }
    return handle;
}

/// The last path component, for the rules that are about a file's own name
/// rather than where it sits.
[[nodiscard]] std::string_view baseName(std::string_view name) {
    const std::size_t slash = name.find_last_of("/\\");
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

}  // namespace

ArchivePtr openArchiveFile(const std::filesystem::path& path) {
    ArchivePtr handle = newReader();
    if (!handle) {
        return {nullptr, &archive_read_free};
    }

#ifdef _WIN32
    const int status =
        archive_read_open_filename_w(handle.get(), path.c_str(), kBlockSize);
#else
    const int status =
        archive_read_open_filename(handle.get(), path.c_str(), kBlockSize);
#endif
    if (status != ARCHIVE_OK) {
        return {nullptr, &archive_read_free};
    }
    return handle;
}

ArchivePtr openArchiveMemory(std::span<const std::byte> bytes) {
    ArchivePtr handle = newReader();
    if (!handle || bytes.empty()) {
        return {nullptr, &archive_read_free};
    }
    if (archive_read_open_memory(handle.get(), bytes.data(), bytes.size()) !=
        ARCHIVE_OK) {
        return {nullptr, &archive_read_free};
    }
    return handle;
}

std::string entryName(struct archive_entry* entry) {
    if (const char* utf8 = archive_entry_pathname_utf8(entry); utf8 != nullptr) {
        return utf8;
    }
    if (const char* raw = archive_entry_pathname(entry); raw != nullptr) {
        return toUtf8(raw);
    }
    return {};
}

std::optional<std::vector<std::byte>> readEntry(struct archive* handle,
                                                std::int64_t    sizeHint) {
    std::vector<std::byte> data;
    if (sizeHint > 0) {
        data.reserve(static_cast<std::size_t>(sizeHint));
    }

    std::byte chunk[kBlockSize];
    for (;;) {
        const la_ssize_t got = archive_read_data(handle, chunk, sizeof(chunk));
        if (got == 0) {
            return data;  // end of the entry
        }
        if (got < 0) {
            return std::nullopt;
        }
        data.insert(data.end(), chunk, chunk + got);
    }
}

bool isArchiveJunk(std::string_view name) {
    if (name.starts_with("__MACOSX/")) {
        return true;
    }
    const std::string_view base = baseName(name);
    // AppleDouble sidecars, which carry the resource fork of the file they are
    // named after and nothing playable.
    return base.starts_with("._") || base == ".DS_Store" || base == "Thumbs.db";
}

}  // namespace xpcog::codecs
