#include "PsfFile.hpp"

#include "common/TextEncoding.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/Plugin.hpp"

extern "C" {
#include <psflib/psflib.h>
}

#include <charconv>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::codecs {
namespace {

/// psflib works in stdio terms and opens files by name, which is how it follows
/// a `_lib` tag. Each handle is one ISource obtained from the registry, so a PSF
/// inside an archive resolves its libraries from that same archive rather than
/// from the filesystem the archive happens to sit on.
struct PsfHandle {
    SourcePtr source;
};

/// Shared by every callback: the registry to open through, and the directory the
/// outermost file came from, since `_lib` names are relative to it.
struct PsfContext {
    const PluginRegistry* registry = nullptr;
    Url                   base;
};

thread_local PsfContext* tlsContext = nullptr;

/// psflib's fopen takes only a name, with no user pointer, which is why the
/// context is thread-local rather than passed. Loading is confined to one call
/// on one thread, so the scope is exactly the psf_load() below.
struct ScopedContext {
    explicit ScopedContext(PsfContext* context) { tlsContext = context; }
    ~ScopedContext() { tlsContext = nullptr; }
    ScopedContext(const ScopedContext&)            = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
};

void* psfOpen(const char* path) {
    if (tlsContext == nullptr || path == nullptr) {
        return nullptr;
    }

    // psflib hands back the name from the `_lib` tag, joined onto the directory
    // of the file that named it. It is a filesystem path, so it becomes a URL
    // the same way any other local path does.
    const Url url = Url::fromLocalPath(pathFromUtf8(path));

    auto handle    = std::make_unique<PsfHandle>();
    handle->source = tlsContext->registry->makeSource(url);
    if (!handle->source || !handle->source->open(url)) {
        return nullptr;  // a library the chain names but nothing holds
    }
    return handle.release();
}

std::size_t psfRead(void* buffer, std::size_t size, std::size_t count, void* handle) {
    auto* self = static_cast<PsfHandle*>(handle);
    if (self == nullptr || size == 0) {
        return 0;
    }
    const std::int64_t got =
        self->source->read(buffer, static_cast<std::int64_t>(size * count));
    return got <= 0 ? 0 : static_cast<std::size_t>(got) / size;
}

int psfSeek(void* handle, std::int64_t offset, int whence) {
    auto* self = static_cast<PsfHandle*>(handle);
    return (self != nullptr && self->source->seek(offset, whence)) ? 0 : -1;
}

int psfClose(void* handle) {
    delete static_cast<PsfHandle*>(handle);
    return 0;
}

long psfTell(void* handle) {
    auto* self = static_cast<PsfHandle*>(handle);
    return self == nullptr ? -1 : static_cast<long>(self->source->tell());
}

constexpr psf_file_callbacks kCallbacks = {
    // Both separators, always. A PSF written on Windows names its library with a
    // backslash and one written anywhere else uses a slash, and the same file
    // gets read on both.
    "\\/",
    &psfOpen, &psfRead, &psfSeek, &psfClose, &psfTell,
};

}  // namespace

std::optional<double> parsePsfTime(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    double total = 0.0;
    double part  = 0.0;
    bool   any   = false;
    std::string digits;

    const auto flush = [&] {
        if (digits.empty()) {
            return false;
        }
        double value = 0.0;
        try {
            value = std::stod(digits);
        } catch (...) {
            return false;
        }
        total = total * 60.0 + part;
        part  = value;
        any   = true;
        digits.clear();
        return true;
    };

    for (const char c : text) {
        if (c == ':') {
            if (!flush()) {
                return std::nullopt;
            }
        } else if ((c >= '0' && c <= '9') || c == '.') {
            digits.push_back(c);
        } else if (c != ' ') {
            return std::nullopt;
        }
    }
    if (!flush() || !any) {
        return std::nullopt;
    }
    return total * 60.0 + part;
}

namespace {

/// Collects what the outermost file's tag block says.
int psfInfo(void* context, const char* name, const char* value) {
    auto* file = static_cast<PsfFile*>(context);
    if (file == nullptr || name == nullptr || value == nullptr) {
        return 0;
    }

    const std::string key  = codecs::toUtf8(name);
    const std::string text = codecs::toUtf8(value);

    if (key == "length") {
        file->length = parsePsfTime(text);
    } else if (key == "fade") {
        file->fade = parsePsfTime(text);
    } else if (key == "volume") {
        try {
            file->volume = std::stod(text);
        } catch (...) {
            file->volume = 1.0;
        }
    }

    // Kept whatever it is, including length and fade: the info panel showing
    // what the file actually says beats it showing only what we understood.
    file->tags.set(key, text);
    return 0;
}

int psfLoadProgram(void* context, const std::uint8_t* exe, std::size_t exeSize,
                   const std::uint8_t* reserved, std::size_t reservedSize) {
    auto* file = static_cast<PsfFile*>(context);
    if (file == nullptr) {
        return -1;
    }

    PsfProgram program;
    if (exe != nullptr && exeSize > 0) {
        program.exe.assign(exe, exe + exeSize);
    }
    if (reserved != nullptr && reservedSize > 0) {
        program.reserved.assign(reserved, reserved + reservedSize);
    }
    file->programs.push_back(std::move(program));
    return 0;
}

[[nodiscard]] std::optional<PsfFile> load(const Url& url,
                                          const PluginRegistry& registry,
                                          std::uint8_t allowedVersion, bool wantPrograms) {
    const auto path = url.localPath();
    if (!path) {
        // psflib addresses libraries by path, so the chain has to start from one.
        // A PSF over HTTP with no `_lib` would work, but one with a library
        // would silently lose it, and half a program image is not worth serving.
        return std::nullopt;
    }

    PsfContext context;
    context.registry = &registry;
    context.base     = url;
    const ScopedContext scope{&context};

    PsfFile file;
    const int version = psf_load(pathToUtf8(*path).c_str(), &kCallbacks, allowedVersion,
                                 wantPrograms ? &psfLoadProgram : nullptr,
                                 wantPrograms ? &file : nullptr, &psfInfo, &file,
                                 /*info_want_nested_tags=*/0);
    if (version <= 0) {
        return std::nullopt;
    }

    file.version = static_cast<std::uint8_t>(version);
    return file;
}

}  // namespace

std::optional<PsfFile> loadPsf(const Url& url, const PluginRegistry& registry,
                               std::uint8_t allowedVersion) {
    return load(url, registry, allowedVersion, /*wantPrograms=*/true);
}

std::optional<PsfFile> readPsfTags(const Url& url, const PluginRegistry& registry) {
    return load(url, registry, /*allowedVersion=*/0, /*wantPrograms=*/false);
}

}  // namespace xpcog::codecs
