// Replaces Cog's runtime NSBundle + conformsToProtocol: discovery
// (Cog Audio/PluginController.mm) with compile-time registration.
//
// Codecs do not self-register. Each exposes exactly one registrar function which
// the generated codecs/RegisterAll.cpp calls in a deterministic order. See
// cmake/XPCogCodec.cmake for why self-registering statics are a trap here.

#pragma once

#include "xpcog/core/Plugin.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace xpcog {

// Plugin.hpp is included rather than forward-declaring ISource/IDecoder:
// std::unique_ptr's default deleter needs a complete type.
using SourcePtr  = std::unique_ptr<ISource>;
using DecoderPtr = std::unique_ptr<IDecoder>;

// libc++ accepts an incomplete type here in contexts where libstdc++ and MinGW
// reject it, so a forward declaration compiles on macOS and then breaks the Linux
// and Windows builds. These assertions fail identically on every compiler.
static_assert(sizeof(ISource) > 0, "ISource must be a complete type for SourcePtr");
static_assert(sizeof(IDecoder) > 0, "IDecoder must be a complete type for DecoderPtr");
static_assert(std::has_virtual_destructor_v<ISource>,
              "ISource is deleted through a base pointer");
static_assert(std::has_virtual_destructor_v<IDecoder>,
              "IDecoder is deleted through a base pointer");

/// Mirrors Cog's `+priority` (Audio/Plugin.h): higher wins, 1.0 is the default.
/// Candidates are tried in descending priority, ties broken by registration order.
using Priority = float;
inline constexpr Priority kDefaultPriority = 1.0f;

struct DecoderDescriptor {
    std::string_view name;      ///< e.g. "FlacDecoder"; also the skip-cue filter key
    Priority         priority = kDefaultPriority;
    std::span<const std::string_view> extensions;  ///< lowercase, no dot
    std::span<const std::string_view> mimeTypes;   ///< lowercase
    DecoderPtr (*create)() = nullptr;
    /// Replaces Cog's CogVersionCheck `+shouldLoadForOSVersion:`. Evaluated during
    /// freeze(); a descriptor answering false is dropped.
    bool (*available)() = nullptr;
};

/// Expands a playlist or archive into the tracks it contains. The registry opens
/// `source` and hands it over, so containers never open files themselves --
/// unlike Cog's CogContainer, which reaches for AudioSource internally. Taking
/// the source as a parameter keeps containers testable without the filesystem.
using ContainerExpandFn = std::vector<Url> (*)(const Url& url, ISource& source);

struct ContainerDescriptor {
    std::string_view                  name;
    Priority                          priority = kDefaultPriority;
    std::span<const std::string_view> extensions;
    std::span<const std::string_view> mimeTypes;
    ContainerExpandFn                 expand = nullptr;
    /// Files the container needs alongside it (a cue sheet's audio file).
    /// Optional; may be null. Mirrors Cog's +dependencyUrlsForContainerURL:.
    ContainerExpandFn dependencies = nullptr;
    bool (*available)()            = nullptr;
};

struct SourceDescriptor {
    std::string_view name;
    Priority         priority = kDefaultPriority;
    std::span<const std::string_view> schemes;     ///< lowercase, no colon
    SourcePtr (*create)() = nullptr;
    bool (*available)()   = nullptr;
};

/// Whether to exclude the cue-sheet decoder from candidate selection, so that
/// resolving a cue's referenced audio file does not recurse back into the cue.
/// Mirrors the `skipCue:` argument threaded through Cog's PluginController.
enum class SkipCue : std::uint8_t { No, Yes };

class PluginRegistry {
public:
    PluginRegistry();
    ~PluginRegistry();

    PluginRegistry(const PluginRegistry&)            = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    void addSource(SourceDescriptor);
    void addDecoder(DecoderDescriptor);
    void addContainer(ContainerDescriptor);

    /// Builds the extension/MIME lookup buckets and sorts each by descending
    /// priority. Must be called once after registration; mutators assert afterwards.
    void freeze();
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    [[nodiscard]] std::size_t decoderCount() const noexcept { return decoders_.size(); }
    [[nodiscard]] std::size_t sourceCount()  const noexcept { return sources_.size(); }
    [[nodiscard]] std::size_t containerCount() const noexcept { return containers_.size(); }

    /// True when some container claims this URL's extension.
    [[nodiscard]] bool isContainer(const Url& url) const;

    /// Expands a playlist or archive into its tracks. Returns just `url` when no
    /// container claims it, so callers can apply this uniformly to any input.
    [[nodiscard]] std::vector<Url> expandContainer(const Url& url) const;

    /// Creates a source for `url`'s scheme, or nullptr if none claims it.
    /// The source is NOT opened.
    [[nodiscard]] SourcePtr makeSource(const Url& url) const;

    /// Reproduces Cog Audio/PluginController.mm:630-668 -- match on lowercased
    /// extension first, then on the source's MIME type. Returns nullptr when
    /// nothing claims the input; the SilenceDecoder fallback arrives in M1b.
    ///
    /// When several decoders claim the input they are returned wrapped so each is
    /// tried in descending priority order, matching Cog's CogDecoderMulti.
    [[nodiscard]] DecoderPtr makeDecoder(const ISource& source,
                                         SkipCue skipCue = SkipCue::No) const;

    /// Opens `url` and returns a source/decoder pair ready to read, or {nullptr,
    /// nullptr} on failure. The decoder borrows the source, so the returned pair
    /// must be kept together and destroyed decoder-first.
    struct OpenResult {
        SourcePtr  source;
        DecoderPtr decoder;
        [[nodiscard]] explicit operator bool() const noexcept {
            return source != nullptr && decoder != nullptr;
        }
    };
    [[nodiscard]] OpenResult open(const Url& url, SkipCue skipCue = SkipCue::No) const;

    /// Every extension claimed by a registered decoder, lowercase and deduplicated.
    /// Drives the app's file-open filter, replacing Cog's generated
    /// CFBundleDocumentTypes (Audio/PluginController.mm -printPluginInfo).
    [[nodiscard]] std::span<const std::string> allExtensions() const noexcept;

private:
    std::vector<SourceDescriptor>    sources_;
    std::vector<DecoderDescriptor>   decoders_;
    std::vector<ContainerDescriptor> containers_;
    std::vector<std::string>       allExtensions_;
    bool                           frozen_ = false;
};

/// Wraps several candidates so each is tried in order until one opens.
/// Port of Cog's CogDecoderMulti; see core/src/MultiDecoder.cpp.
/// The descriptors must outlive the returned decoder, which the registry
/// guarantees since they live in the frozen registry itself.
[[nodiscard]] DecoderPtr makeMultiDecoder(
    std::vector<const DecoderDescriptor*> candidates);

/// Defined by the generated codecs/RegisterAll.cpp. Registers every codec compiled
/// into this build and calls freeze().
void registerAllCodecs(PluginRegistry&);

}  // namespace xpcog
