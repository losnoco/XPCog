// Which channels the spectrum and the oscilloscope show, and the spelling
// each mode has in its setting.
//
// The two lists are the same five words plus "mirrored" for the spectrum, and
// they are here rather than on the panels because two frontends draw the
// same settings: a spelling the wx panel writes has to be one the GTK view
// reads, and one table each is how they cannot disagree.

#pragma once

#include <string_view>

namespace xpcog::app {

/// The `spectrumChannels` setting, decoded.
enum class SpectrumChannels { Mono, Left, Right, Mirrored, Stacked, Overlaid };

/// The `scopeChannels` setting, decoded.
enum class ScopeChannels { Mono, Left, Right, Stacked, Overlaid };

/// The setting's spelling of each, in the order the menus and the panes list
/// them, and the reverse: an unknown spelling reads as Mono.
[[nodiscard]] const char*      spectrumChannelsKey(SpectrumChannels channels) noexcept;
[[nodiscard]] SpectrumChannels spectrumChannelsFromKey(std::string_view key) noexcept;
[[nodiscard]] const char*      scopeChannelsKey(ScopeChannels channels) noexcept;
[[nodiscard]] ScopeChannels    scopeChannelsFromKey(std::string_view key) noexcept;

/// Whether a mode reads both sides, and so needs both lanes of the tap.
[[nodiscard]] constexpr bool stereo(SpectrumChannels channels) noexcept {
    return channels == SpectrumChannels::Mirrored || channels == SpectrumChannels::Stacked ||
           channels == SpectrumChannels::Overlaid;
}

}  // namespace xpcog::app
