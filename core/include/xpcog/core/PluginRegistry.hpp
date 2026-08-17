// Replaces Cog's runtime NSBundle + conformsToProtocol: discovery
// (Cog Audio/PluginController.mm) with compile-time registration.
//
// Codecs do not self-register. Each exposes exactly one registrar function which
// the generated codecs/RegisterAll.cpp calls in a deterministic order. See
// cmake/XPCogCodec.cmake for why self-registering statics are a trap here.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {

class ISource;
class IDecoder;

using SourcePtr  = std::unique_ptr<ISource>;
using DecoderPtr = std::unique_ptr<IDecoder>;

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

    /// Builds the extension/MIME lookup buckets and sorts each by descending
    /// priority. Must be called once after registration; mutators assert afterwards.
    void freeze();
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    [[nodiscard]] std::size_t decoderCount() const noexcept { return decoders_.size(); }
    [[nodiscard]] std::size_t sourceCount()  const noexcept { return sources_.size(); }

    /// Every extension claimed by a registered decoder, lowercase and deduplicated.
    /// Drives the app's file-open filter, replacing Cog's generated
    /// CFBundleDocumentTypes (Audio/PluginController.mm -printPluginInfo).
    [[nodiscard]] std::span<const std::string> allExtensions() const noexcept;

private:
    std::vector<SourceDescriptor>  sources_;
    std::vector<DecoderDescriptor> decoders_;
    std::vector<std::string>       allExtensions_;
    bool                           frozen_ = false;
};

/// Defined by the generated codecs/RegisterAll.cpp. Registers every codec compiled
/// into this build and calls freeze().
void registerAllCodecs(PluginRegistry&);

}  // namespace xpcog
