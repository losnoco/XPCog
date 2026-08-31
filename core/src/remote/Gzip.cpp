#include "Gzip.hpp"

#include <zlib.h>

#include <array>
#include <cstring>

namespace xpcog::remote {

bool inflateGzip(std::span<const std::byte> compressed, std::string& out) {
    out.clear();
    if (compressed.empty()) {
        return false;
    }

    z_stream stream{};
    // 16 + MAX_WBITS asks for a gzip wrapper rather than a raw or zlib one,
    // which is what these files carry.
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        return false;
    }

    stream.next_in = reinterpret_cast<Bytef*>(
        const_cast<std::byte*>(compressed.data()));  // NOLINT: zlib's API is non-const
    stream.avail_in = static_cast<uInt>(compressed.size());

    std::array<char, 64 * 1024> buffer{};
    int                         status = Z_OK;
    do {
        stream.next_out  = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());

        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&stream);
            out.clear();
            return false;
        }
        out.append(buffer.data(), buffer.size() - stream.avail_out);
    } while (status != Z_STREAM_END);

    inflateEnd(&stream);
    return true;
}

}  // namespace xpcog::remote
