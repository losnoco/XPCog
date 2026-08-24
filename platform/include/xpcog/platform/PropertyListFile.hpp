// Reading a property list that may be in Apple's binary format.
//
// Core has a plist reader already (`PropertyList.hpp`), and it is XML-only,
// deliberately: it exists to read Cog's XML playlists, which are the format
// `NSPropertyListXMLFormat_v1_0` writes. A *preferences* file is the other one.
// `org.cogx.cog.plist` is `bplist00`, because that is what `NSUserDefaults`
// writes, and no amount of XML parsing will read it.
//
// Rather than grow a second parser in core, this converts. The binary format is
// Apple's and `CFPropertyList` reads it on the platform that defines it, so the
// macOS-only part of the job is a format change of about thirty lines and
// everything downstream -- the mapping from Cog's keys to XPCog's, which is the
// part with decisions in it -- stays portable, testable, and in core.
//
// It is in `platform/` rather than `core/` for the reason everything here is:
// core links no OS framework, and this needs CoreFoundation. The public header
// names none, so the layering check is satisfied for the same reason the media
// integrations satisfy it.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace xpcog::platform {

/// The property list at `path`, as XML text.
///
/// A file already in XML is returned as it stands, on every platform -- so the
/// XML case is not macOS-only and can be tested anywhere. A binary one needs
/// `CFPropertyList` and therefore answers nullopt off macOS, which is not a
/// limitation worth apologising for: the only binary plist XPCog has a reason to
/// read belongs to a program that runs nowhere else.
///
/// nullopt for a file that will not open, is not a property list, or is binary
/// on a platform that cannot read one. The three are not distinguished, because
/// every caller's response to them is the same.
[[nodiscard]] std::optional<std::string> propertyListToXml(
    const std::filesystem::path& path);

}  // namespace xpcog::platform
