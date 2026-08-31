// Expanding a gzip member, for the docs assets that are committed compressed.
//
// The two vendored Swagger UI files are stored gzipped and served that way,
// which is what a browser wants and costs the binary a megabyte less. A client
// that never offered to accept gzip has to be given something it can read
// instead, and this is that path. zlib is already a hard dependency of this
// project -- a plain entry in vcpkg.json rather than a feature -- so it costs
// nothing to reach for here.

#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace xpcog::remote {

/// Expands a gzip member into `out`. False when the bytes are not one, which for
/// an asset compiled into this binary means the build is wrong rather than the
/// request.
[[nodiscard]] bool inflateGzip(std::span<const std::byte> compressed, std::string& out);

}  // namespace xpcog::remote
