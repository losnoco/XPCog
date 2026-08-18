#include "CompressedFileSource.hpp"

#include "ArchiveReader.hpp"

#include "common/BlobSource.hpp"
#include "common/SourceBytes.hpp"

#include <archive_entry.h>

#include <memory>
#include <utility>

namespace xpcog::codecs {
namespace {

/// Cog's OpenMPT list, minus the ones libopenmpt already reads itself.
constexpr std::string_view kExtensions[] = {"mdz",   "mdr", "s3z",
                                            "xmz",   "itz", "mptmz"};

class CompressedFileSource final : public BlobSource {
public:
    explicit CompressedFileSource(SourcePtr inner) : inner_(std::move(inner)) {}

    bool open(const Url& url) override {
        close();
        if (!inner_ || !inner_->open(url)) {
            return false;
        }

        // The compressed bytes whole, then the inner source is done with: what
        // it was wrapping is a few tens of kilobytes, and holding a file handle
        // open for the life of the track buys nothing once it has been read.
        const auto packed = readAllBytes(*inner_);
        inner_->close();
        if (!packed || packed->empty()) {
            return false;
        }

        const ArchivePtr handle = openArchiveMemory(*packed);
        if (!handle) {
            return false;  // not an archive at all, whatever the name promised
        }

        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(handle.get(), &entry) == ARCHIVE_OK) {
            if (archive_entry_filetype(entry) != AE_IFREG) {
                continue;
            }
            const std::string name = entryName(entry);
            if (name.empty() || isArchiveJunk(name)) {
                archive_read_data_skip(handle.get());
                continue;
            }

            // The first real file, without checking what it is called. The
            // extension outside already said what this is; a packer that named
            // the module inside `SONG` or `readme.mod` or nothing at all does
            // not change that, and being strict here would reject files that
            // play perfectly well everywhere else.
            auto member = readEntry(handle.get(), archive_entry_size(entry));
            if (!member || member->empty()) {
                return false;
            }
            setBlob(std::move(*member));
            return true;
        }

        // An archive holding nothing but junk, or nothing at all.
        return false;
    }

    void close() override {
        BlobSource::close();
        if (inner_) {
            inner_->close();
        }
    }

    /// The wrapper is transparent, so it answers with the URL that was opened --
    /// which is also what picks the decoder, and has to stay the compressed
    /// extension for that to land on the module decoder.
    [[nodiscard]] const Url& url() const override {
        static const Url kNone;
        return inner_ ? inner_->url() : kNone;
    }

    /// Deliberately not the inner one. A transport that reports
    /// `application/zip` would be describing the wrapper rather than the audio,
    /// and decoder selection falls back to MIME when the extension misses.
    [[nodiscard]] std::string mimeType() const override { return {}; }

    void interrupt() override {
        if (inner_) {
            inner_->interrupt();
        }
    }

    void setSettings(const Settings* settings) override {
        if (inner_) {
            inner_->setSettings(settings);
        }
    }

private:
    SourcePtr inner_;
};

}  // namespace

std::span<const std::string_view> compressedModuleExtensions() {
    return kExtensions;
}

SourcePtr makeCompressedFileSource(SourcePtr inner) {
    if (!inner) {
        return nullptr;
    }
    return std::make_unique<CompressedFileSource>(std::move(inner));
}

}  // namespace xpcog::codecs
