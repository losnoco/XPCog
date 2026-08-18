// A compressed file that is one file, unwrapped where nobody has to see it.
//
// `.itz` is an IT module inside a zip. `.mdz` is a MOD, `.s3z` an S3M, `.xmz` an
// XM, `.mdr` a MOD inside a RAR. The tracker scene has used these since the
// 1990s and they are, as far as anyone using them is concerned, module files --
// you double-click one and it plays.
//
// That is why they are not containers here. ArchiveContainer turns a `.zip` into
// one `unpack://` URL per member, which is right for an archive: it holds
// several tracks and the point is to see them. A `.itz` holds one, and expanding
// it would put a row in the playlist named after whatever the packer happened to
// call the file inside, for a file the user thinks of by the name they can see.
// So the URL stays `file:///.../song.itz` and the *source* quietly hands the
// decoder the module rather than the zip.
//
// This works because the outer extension already names what is inside: a `.itz`
// is an IT and can be nothing else, so the decoder can be chosen before anything
// is unpacked. `.gz` names nothing -- `song.mod.gz` and `rip.spc.gz` are the same
// extension -- which is why gzip stays with ArchiveContainer, where the member's
// own name is what picks the decoder.
//
// Cog claims these extensions on its OpenMPT decoder and unpacks them through
// File_Extractor. Same result, arranged differently: XPCog keeps the unpacking
// in the archive codec, so libarchive stays out of the module decoder and any
// future format with the same one-file-in-a-wrapper convention gets it for free.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"

#include <span>
#include <string_view>

namespace xpcog::codecs {

/// The extensions that mean "exactly one module, compressed".
[[nodiscard]] std::span<const std::string_view> compressedModuleExtensions();

/// Wraps `inner` so that reads see the sole member of the archive it opens.
/// Never null: a wrapper that cannot help returns the source it was given.
[[nodiscard]] SourcePtr makeCompressedFileSource(SourcePtr inner);

}  // namespace xpcog::codecs
