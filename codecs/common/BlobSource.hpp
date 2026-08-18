// An ISource over bytes already held in memory.
//
// Two sources decompress their input whole before serving any of it. A member of
// a solid archive cannot be seeked without decompressing everything ahead of it,
// and a compressed module is a single file that only comes out in one piece --
// so both end up holding a blob and answering reads from it. Once the bytes are
// in hand that is the same problem twice.
//
// A base class rather than a concrete source, because all that differs between
// the two is where the bytes come from and which URL the result answers to.

#pragma once

#include "xpcog/core/Plugin.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace xpcog::codecs {

class BlobSource : public ISource {
public:
    [[nodiscard]] bool seekable() const override { return true; }

    bool seek(std::int64_t position, int whence) override {
        const auto size = static_cast<std::int64_t>(blob_.size());
        switch (whence) {
            case SEEK_CUR: position += offset_; break;
            case SEEK_END: position += size; break;
            default: break;
        }
        if (position < 0) {
            return false;
        }
        offset_ = position;
        // Seeking exactly to the end is legal and reads 0 afterwards; past it is
        // not, which is how a decoder probing for a footer learns the file is
        // shorter than the one it was expecting.
        return offset_ <= size;
    }

    [[nodiscard]] std::int64_t tell() const override { return offset_; }

    std::int64_t read(void* buffer, std::int64_t bytes) override {
        const auto size = static_cast<std::int64_t>(blob_.size());
        if (bytes <= 0 || offset_ >= size) {
            return 0;
        }
        const std::int64_t take = std::min(bytes, size - offset_);
        std::memcpy(buffer, blob_.data() + offset_, static_cast<std::size_t>(take));
        offset_ += take;
        return take;
    }

    void close() override {
        blob_.clear();
        blob_.shrink_to_fit();
        offset_ = 0;
    }

protected:
    /// Hands over the bytes to serve, rewinding to the start. Called from the
    /// derived open() once it has them.
    void setBlob(std::vector<std::byte> bytes) {
        blob_   = std::move(bytes);
        offset_ = 0;
    }

    [[nodiscard]] bool empty() const noexcept { return blob_.empty(); }

private:
    std::vector<std::byte> blob_;
    std::int64_t           offset_ = 0;
};

}  // namespace xpcog::codecs
