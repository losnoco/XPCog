// Port of Cog Plugins/FileSource/FileSource.m.
//
// Two things from the original are deliberately absent:
//   * SandboxBroker begin/endFolderAccess -- macOS App Sandbox only. The seam
//     for it is platform::IFileAccess (M2); nothing is needed here.
//   * File_Extractor (fex) archive handling -- that becomes ArchiveSource in M6,
//     rather than being folded into the plain-file reader as Cog does.
//
// Also fixes a latent bug in the original: -tell and -read dereference the FILE*
// without checking it, so calling them on a failed open crashes rather than
// erroring.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <cstdio>
#include <memory>
#include <string_view>

namespace xpcog {
namespace {

class FileSource final : public ISource {
public:
    ~FileSource() override { FileSource::close(); }

    bool open(const Url& url) override {
        close();

        const auto path = url.localPath();
        if (!path) {
            return false;
        }

#ifdef _WIN32
        // Use the wide-character form so non-ASCII paths work regardless of the
        // active code page.
        file_ = _wfopen(path->c_str(), L"rb");
#else
        file_ = std::fopen(path->c_str(), "rb");
#endif
        if (file_ == nullptr) {
            return false;
        }

        url_ = url;
        return true;
    }

    [[nodiscard]] bool seekable() const override { return file_ != nullptr; }

    bool seek(std::int64_t offset, int whence) override {
        if (file_ == nullptr) {
            return false;
        }
#ifdef _WIN32
        return _fseeki64(file_, offset, whence) == 0;
#else
        return fseeko(file_, static_cast<off_t>(offset), whence) == 0;
#endif
    }

    [[nodiscard]] std::int64_t tell() const override {
        if (file_ == nullptr) {
            return -1;
        }
#ifdef _WIN32
        return _ftelli64(file_);
#else
        return static_cast<std::int64_t>(ftello(file_));
#endif
    }

    std::int64_t read(void* buffer, std::int64_t bytes) override {
        if (file_ == nullptr || bytes <= 0) {
            return 0;
        }
        const std::size_t got =
            std::fread(buffer, 1, static_cast<std::size_t>(bytes), file_);
        if (got == 0 && std::ferror(file_) != 0) {
            return -1;
        }
        return static_cast<std::int64_t>(got);
    }

    void close() override {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    [[nodiscard]] const Url& url() const override { return url_; }

private:
    std::FILE* file_ = nullptr;
    Url        url_;
};

constexpr std::string_view kSchemes[] = {"file"};

}  // namespace
}  // namespace xpcog

void xpcog_register_filesource(xpcog::PluginRegistry& r) {
    r.addSource({
        .name     = "FileSource",
        .priority = xpcog::kDefaultPriority,
        .schemes  = xpcog::kSchemes,
        .create   = []() -> xpcog::SourcePtr {
            return std::make_unique<xpcog::FileSource>();
        },
        .available = nullptr,
    });
}
