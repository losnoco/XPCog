#include "xpcog/core/PluginRegistry.hpp"

#include <algorithm>
#include <cassert>

namespace xpcog {

PluginRegistry::PluginRegistry()  = default;
PluginRegistry::~PluginRegistry() = default;

void PluginRegistry::addSource(SourceDescriptor d) {
    assert(!frozen_ && "PluginRegistry::addSource after freeze()");
    assert(d.create && "source descriptor needs a create function");
    sources_.push_back(d);
}

void PluginRegistry::addDecoder(DecoderDescriptor d) {
    assert(!frozen_ && "PluginRegistry::addDecoder after freeze()");
    assert(d.create && "decoder descriptor needs a create function");
    decoders_.push_back(d);
}

void PluginRegistry::freeze() {
    assert(!frozen_ && "PluginRegistry::freeze() called twice");

    // CogVersionCheck equivalent: drop descriptors that decline to load here, so
    // selection never has to reconsider availability.
    const auto unavailable = [](const auto& d) { return d.available && !d.available(); };
    std::erase_if(sources_, unavailable);
    std::erase_if(decoders_, unavailable);

    // Stable sort by descending priority preserves registration order within a
    // priority band, matching the ordering Cog's PluginController relies on.
    const auto byPriority = [](const auto& a, const auto& b) { return a.priority > b.priority; };
    std::stable_sort(sources_.begin(), sources_.end(), byPriority);
    std::stable_sort(decoders_.begin(), decoders_.end(), byPriority);

    allExtensions_.clear();
    for (const auto& d : decoders_) {
        for (const auto ext : d.extensions) {
            allExtensions_.emplace_back(ext);
        }
    }
    std::sort(allExtensions_.begin(), allExtensions_.end());
    allExtensions_.erase(std::unique(allExtensions_.begin(), allExtensions_.end()),
                         allExtensions_.end());

    frozen_ = true;
}

std::span<const std::string> PluginRegistry::allExtensions() const noexcept {
    return allExtensions_;
}

}  // namespace xpcog
