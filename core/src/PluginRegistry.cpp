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

void PluginRegistry::addContainer(ContainerDescriptor d) {
    assert(!frozen_ && "PluginRegistry::addContainer after freeze()");
    assert(d.expand && "container descriptor needs an expand function");
    containers_.push_back(d);
}

void PluginRegistry::freeze() {
    assert(!frozen_ && "PluginRegistry::freeze() called twice");

    // CogVersionCheck equivalent: drop descriptors that decline to load here, so
    // selection never has to reconsider availability.
    const auto unavailable = [](const auto& d) { return d.available && !d.available(); };
    std::erase_if(sources_, unavailable);
    std::erase_if(decoders_, unavailable);
    std::erase_if(containers_, unavailable);

    // Stable sort by descending priority preserves registration order within a
    // priority band, matching the ordering Cog's PluginController relies on.
    const auto byPriority = [](const auto& a, const auto& b) { return a.priority > b.priority; };
    std::stable_sort(sources_.begin(), sources_.end(), byPriority);
    std::stable_sort(decoders_.begin(), decoders_.end(), byPriority);
    std::stable_sort(containers_.begin(), containers_.end(), byPriority);

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

bool PluginRegistry::isContainer(const Url& url) const {
    const std::string extension = url.extension();
    if (extension.empty()) {
        return false;
    }
    for (const auto& descriptor : containers_) {
        for (const auto claimed : descriptor.extensions) {
            if (claimed == extension) {
                return true;
            }
        }
    }
    return false;
}

std::vector<Url> PluginRegistry::expandContainer(const Url& url) const {
    const std::string extension = url.extension();

    for (const auto& descriptor : containers_) {
        for (const auto claimed : descriptor.extensions) {
            if (claimed != extension) {
                continue;
            }
            SourcePtr source = makeSource(url);
            if (!source || !source->open(url)) {
                return {};
            }
            return descriptor.expand(url, *source);
        }
    }

    // Not a container: it is its own single track.
    return {url};
}

SourcePtr PluginRegistry::makeSource(const Url& url) const {
    const std::string_view scheme = url.scheme();

    for (const auto& descriptor : sources_) {
        for (const auto claimed : descriptor.schemes) {
            if (claimed == scheme) {
                return descriptor.create();
            }
        }
    }
    return nullptr;
}

DecoderPtr PluginRegistry::makeDecoder(const ISource& source, SkipCue skipCue) const {
    const auto matching = [&](auto&& claims, std::string_view key) {
        std::vector<const DecoderDescriptor*> found;
        if (key.empty()) {
            return found;
        }
        for (const auto& descriptor : decoders_) {
            if (skipCue == SkipCue::Yes && descriptor.name == "CueSheetDecoder") {
                continue;
            }
            for (const auto claimed : claims(descriptor)) {
                if (claimed == key) {
                    found.push_back(&descriptor);
                    break;
                }
            }
        }
        return found;
    };

    // Extension first, then MIME type -- the order Cog uses.
    auto candidates = matching([](const DecoderDescriptor& d) { return d.extensions; },
                               source.url().extension());
    if (candidates.empty()) {
        const std::string mime = source.mimeType();
        candidates =
            matching([](const DecoderDescriptor& d) { return d.mimeTypes; }, mime);
    }

    if (candidates.empty()) {
        return nullptr;
    }
    if (candidates.size() == 1) {
        return candidates.front()->create();
    }
    return makeMultiDecoder(std::move(candidates));
}

PluginRegistry::OpenResult PluginRegistry::open(const Url& url, SkipCue skipCue) const {
    OpenResult result;

    result.source = makeSource(url);
    if (!result.source || !result.source->open(url)) {
        return {};
    }

    result.decoder = makeDecoder(*result.source, skipCue);
    if (!result.decoder || !result.decoder->open(result.source.get())) {
        return {};
    }

    return result;
}

}  // namespace xpcog
