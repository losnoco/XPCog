// libarchive, wrapped just enough that two sources can share it.
//
// Nothing here is a policy decision: it is the handle lifetime, the entry name,
// the read loop, and the one rule about which entries are not files at all. What
// to *do* with the entries is ArchiveSource's business (offer each playable one
// as a track) or CompressedFileSource's (there is only one, unwrap it).

#pragma once

#include <archive.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct archive_entry;

namespace xpcog::codecs {

/// libarchive's reader, closed and freed however the scope ends.
using ArchivePtr = std::unique_ptr<struct archive, decltype(&archive_read_free)>;

/// Opens an archive on disk. Null if it cannot be read as one.
///
/// Every format and every compression filter the build supports is enabled: the
/// extension is a hint from the file's name, and `rsn` (a RAR of SPC rips) and
/// `vgm7z` are proof that the name can be anything at all.
[[nodiscard]] ArchivePtr openArchiveFile(const std::string& path);

/// The same, over bytes already in memory. `bytes` must outlive the handle --
/// libarchive reads from the caller's buffer rather than copying it.
[[nodiscard]] ArchivePtr openArchiveMemory(std::span<const std::byte> bytes);

/// The entry's name, preferring libarchive's UTF-8 accessor and falling back to
/// the raw bytes put through the same guess every other untagged text here gets.
[[nodiscard]] std::string entryName(struct archive_entry* entry);

/// Reads the current entry whole. `sizeHint` only reserves: archive_entry_size
/// is unreliable for some formats, so the read loop is the authority. Returns
/// nullopt if the entry errors partway, which for a compressed member means a
/// corrupt archive rather than a short file.
[[nodiscard]] std::optional<std::vector<std::byte>> readEntry(struct archive* handle,
                                                              std::int64_t sizeHint);

/// True for the bookkeeping a macOS-made archive carries beside the real files.
///
/// These matter more than housekeeping suggests: a resource fork is stored as
/// `__MACOSX/._Track.spc`, which ends in a playable extension and would
/// otherwise appear in the playlist as a second copy of every track -- one that
/// no decoder can open.
[[nodiscard]] bool isArchiveJunk(std::string_view name);

}  // namespace xpcog::codecs
