#include "VisualizerChannels.hpp"

#include <array>

namespace xpcog::app {
namespace {

template <typename Channels>
struct ChannelsName {
    Channels    channels;
    const char* key;
};

constexpr std::array<ChannelsName<SpectrumChannels>, 6> kSpectrumNames = {{
    {SpectrumChannels::Mono, "mono"},
    {SpectrumChannels::Left, "left"},
    {SpectrumChannels::Right, "right"},
    {SpectrumChannels::Mirrored, "mirrored"},
    {SpectrumChannels::Stacked, "stacked"},
    {SpectrumChannels::Overlaid, "overlaid"},
}};

constexpr std::array<ChannelsName<ScopeChannels>, 5> kScopeNames = {{
    {ScopeChannels::Mono, "mono"},
    {ScopeChannels::Left, "left"},
    {ScopeChannels::Right, "right"},
    {ScopeChannels::Stacked, "stacked"},
    {ScopeChannels::Overlaid, "overlaid"},
}};

template <typename Table, typename Channels>
const char* keyOf(const Table& table, Channels channels) noexcept {
    for (const auto& name : table) {
        if (name.channels == channels) {
            return name.key;
        }
    }
    return "mono";
}

template <typename Table, typename Channels>
Channels fromKey(const Table& table, std::string_view key) noexcept {
    for (const auto& name : table) {
        if (key == name.key) {
            return name.channels;
        }
    }
    return Channels::Mono;
}

}  // namespace

const char* spectrumChannelsKey(SpectrumChannels channels) noexcept {
    return keyOf(kSpectrumNames, channels);
}

SpectrumChannels spectrumChannelsFromKey(std::string_view key) noexcept {
    return fromKey<decltype(kSpectrumNames), SpectrumChannels>(kSpectrumNames, key);
}

const char* scopeChannelsKey(ScopeChannels channels) noexcept {
    return keyOf(kScopeNames, channels);
}

ScopeChannels scopeChannelsFromKey(std::string_view key) noexcept {
    return fromKey<decltype(kScopeNames), ScopeChannels>(kScopeNames, key);
}

}  // namespace xpcog::app
