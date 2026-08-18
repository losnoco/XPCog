#include "xpcog/core/PluginRegistry.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

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

void PluginRegistry::addMetadataReader(MetadataReaderDescriptor d) {
    assert(!frozen_ && "PluginRegistry::addMetadataReader after freeze()");
    assert(d.read && "metadata reader descriptor needs a read function");
    metadataReaders_.push_back(d);
}

void PluginRegistry::freeze() {
    assert(!frozen_ && "PluginRegistry::freeze() called twice");

    // CogVersionCheck equivalent: drop descriptors that decline to load here, so
    // selection never has to reconsider availability.
    const auto unavailable = [](const auto& d) { return d.available && !d.available(); };
    std::erase_if(sources_, unavailable);
    std::erase_if(decoders_, unavailable);
    std::erase_if(containers_, unavailable);
    std::erase_if(metadataReaders_, unavailable);

    // Stable sort by descending priority preserves registration order within a
    // priority band, matching the ordering Cog's PluginController relies on.
    const auto byPriority = [](const auto& a, const auto& b) { return a.priority > b.priority; };
    std::stable_sort(sources_.begin(), sources_.end(), byPriority);
    std::stable_sort(decoders_.begin(), decoders_.end(), byPriority);
    std::stable_sort(containers_.begin(), containers_.end(), byPriority);
    std::stable_sort(metadataReaders_.begin(), metadataReaders_.end(), byPriority);

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

MetadataMap PluginRegistry::readMetadata(const Url& url) const {
    const std::string extension = url.extension();

    // Ascending priority, because mergeFrom() overwrites: the last reader to
    // speak wins, and that should be the one with the most authority.
    MetadataMap merged;
    for (auto it = metadataReaders_.rbegin(); it != metadataReaders_.rend(); ++it) {
        const bool claims =
            it->extensions.empty() ||
            std::any_of(it->extensions.begin(), it->extensions.end(),
                        [&extension](std::string_view claimed) {
                            return claimed == extension;
                        });
        if (claims) {
            merged.mergeFrom(it->read(url));
        }
    }
    return merged;
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

    // Extension wins over MIME, just as it does for decoders. Apart from
    // preserving Cog's selection rule, this avoids opening every ordinary file
    // merely to find out that it is not a container.
    for (const auto& descriptor : containers_) {
        for (const auto claimed : descriptor.extensions) {
            if (claimed != extension) {
                continue;
            }
            SourcePtr source = makeSource(url);
            if (!source || !source->open(url)) {
                return {};
            }
            return descriptor.expand(url, *source, *this);
        }
    }

    // A station directory commonly redirects a pretty, extensionless URL to a
    // PLS or M3U response. Only the opened source knows its Content-Type, so MIME
    // matching necessarily happens after the extension pass and after open().
    SourcePtr source = makeSource(url);
    if (!source || !source->open(url)) {
        // No extension claimed the URL, so an open failure does not prove it was
        // a broken container. Preserve the ordinary non-container contract.
        return {url};
    }

    const std::string mime = source->mimeType();
    if (!mime.empty()) {
        for (const auto& descriptor : containers_) {
            for (const auto claimed : descriptor.mimeTypes) {
                if (claimed == mime) {
                    return descriptor.expand(url, *source, *this);
                }
            }
        }
    }

    // Not a container: it is its own single track.
    return {url};
}

bool PluginRegistry::isPlayableExtension(std::string_view extension) const noexcept {
    // allExtensions() is sorted and deduplicated by freeze().
    return std::binary_search(allExtensions_.begin(), allExtensions_.end(), extension);
}

SourcePtr PluginRegistry::makeSource(const Url& url) const {
    const std::string_view scheme = url.scheme();

    for (const auto& descriptor : sources_) {
        for (const auto claimed : descriptor.schemes) {
            if (claimed == scheme) {
                SourcePtr source = descriptor.create();
                if (source) {
                    source->setSettings(settings_);
                }
                return source;
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
        DecoderPtr decoder = candidates.front()->create();
        if (decoder) {
            decoder->setRegistry(this);
            decoder->setSettings(settings_);
        }
        return decoder;
    }
    DecoderPtr multi = makeMultiDecoder(std::move(candidates));
    if (multi) {
        multi->setRegistry(this);
        multi->setSettings(settings_);
    }
    return multi;
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
