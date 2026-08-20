// Finding playable files in a PSF corpus.
//
// Every PSF format's tests want the same thing -- a handful of rips, spread
// across as many games as the corpus can offer -- and until this existed each
// of the nine test files answered it with its own copy of the same walk. The
// copies had already drifted: test_psf.cpp's extension list held only the
// first three formats ported and was never extended, so a corpus of QSF rips
// looked empty to it.
//
// The part that is not merely deduplication is `psfChainResolves()`. A corpus
// legitimately contains files that cannot play: a `.minipsf` is a few hundred
// bytes of override that means nothing without the `.psflib` it names, and a
// collection organised for testing keeps a directory of exactly those with
// their libraries removed. The finders took the first match they walked into
// and required audio from it, so pointing these tests at a real collection
// failed nine cases on files that are *supposed* to be unplayable.
//
// Asking is one call to loadPsf(), rather than a cheaper hand-rolled walk of
// the `_lib` tags, so that "playable" here cannot come to mean something
// different from what the decoder does. It costs an inflate per candidate; a
// finder looks at a handful.

#pragma once

#include "psf/PsfFile.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace xpcog::testing {

namespace fs = std::filesystem;

#ifdef XPCOG_PSF_CORPUS
inline constexpr bool kHavePsfCorpus = true;
[[nodiscard]] inline fs::path psfCorpusRoot() { return fs::path{XPCOG_PSF_CORPUS}; }
#else
inline constexpr bool kHavePsfCorpus = false;
[[nodiscard]] inline fs::path psfCorpusRoot() { return {}; }
#endif

/// True when there is a corpus and it is where the cache variable says.
[[nodiscard]] inline bool psfCorpusPresent() {
    return kHavePsfCorpus && fs::exists(psfCorpusRoot());
}

/// One registry for every PSF test in the binary, rather than one per file.
/// Registration is not cheap and the nine copies were identical.
[[nodiscard]] inline PluginRegistry& psfRegistry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

[[nodiscard]] inline std::string lowerExtension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

/// How many program images this file's chain yields: 0 when it does not
/// resolve, 1 when the file is self-contained, more when it named a library.
///
/// More than one is the only dependable way to ask "does this file need a
/// library". The `mini` spelling is a convention that a corpus is free not to
/// follow, and a directory of self-contained `.2sf` rips sorts before the
/// `.mini2sf` ones -- so a test that assumed the spelling got a file that
/// survives being carried away from its library, which is the opposite of what
/// it was checking.
[[nodiscard]] inline std::size_t psfChainLength(const fs::path& path) {
    const auto file = codecs::loadPsf(Url::fromLocalPath(path), psfRegistry());
    return file ? file->programs.size() : 0;
}

/// Whether this file's `_lib` chain resolves -- which is the difference between
/// a rip and an orphan, and is not visible from the file name or its size.
[[nodiscard]] inline bool psfChainResolves(const fs::path& path) {
    return psfChainLength(path) > 0;
}

struct PsfSearch {
    /// How many to return; 0 means every match.
    std::size_t want = 0;

    /// Take at most one file per directory. A game's rips share a library and
    /// therefore share everything a library decides -- six files from one set
    /// test one cartridge six times.
    bool onePerDirectory = false;

    /// Only files that name a library and resolve it. What the tests about the
    /// `_lib` chain need, including the ones that build their own orphan by
    /// copying a file away from the library it names.
    bool chainedOnly = false;
};

/// Playable files with one of `extensions`, in walk order.
///
/// `extensions` include the dot and must be lowercase: {".gsf", ".minigsf"}.
[[nodiscard]] inline std::vector<fs::path> findPsfFiles(
    std::initializer_list<std::string_view> extensions, PsfSearch options = {}) {
    std::vector<fs::path> found;
    if (!kHavePsfCorpus) {
        return found;
    }

    std::error_code                error;
    fs::recursive_directory_iterator walk{
        psfCorpusRoot(), fs::directory_options::skip_permission_denied, error};
    if (error) {
        return found;
    }

    fs::path lastDirectory;
    for (const fs::directory_entry& entry : walk) {
        if (options.want != 0 && found.size() >= options.want) {
            break;
        }
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const std::string extension = lowerExtension(entry.path());
        if (std::find(extensions.begin(), extensions.end(), extension) ==
            extensions.end()) {
            continue;
        }
        if (options.onePerDirectory && entry.path().parent_path() == lastDirectory) {
            continue;
        }
        const std::size_t programs = psfChainLength(entry.path());
        if (programs == 0 || (options.chainedOnly && programs < 2)) {
            continue;
        }
        // Only after the file is known to be usable, or one orphan at the top of
        // a directory would suppress every good file below it.
        lastDirectory = entry.path().parent_path();
        found.push_back(entry.path());
    }
    return found;
}

/// The same, grouped by the directory each file sits in, for the cases that
/// need two rips known to share a library. A directory whose files are all
/// orphans does not appear at all, rather than appearing empty.
[[nodiscard]] inline std::map<fs::path, std::vector<fs::path>> findPsfSets(
    std::initializer_list<std::string_view> extensions) {
    std::map<fs::path, std::vector<fs::path>> sets;
    for (const fs::path& path : findPsfFiles(extensions)) {
        sets[path.parent_path()].push_back(path);
    }
    for (auto& [directory, files] : sets) {
        (void)directory;
        std::sort(files.begin(), files.end());
    }
    return sets;
}

/// Every spelling the PSF container covers, for the tests that are about the
/// container rather than about one console.
[[nodiscard]] inline std::vector<fs::path> findMiniPsfs(std::size_t want) {
    return findPsfFiles({".minipsf", ".minipsf2", ".minissf", ".minidsf",
                         ".miniusf", ".minigsf", ".minisnsf", ".mini2sf",
                         ".minincsf", ".miniqsf"},
                        {.want = want});
}

}  // namespace xpcog::testing
