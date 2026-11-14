// Tag writing back through TagLib, the write-side counterpart of
// TagLibReader.cpp. Declared in the reader's translation unit next to the
// registrar that wires them into a MetadataWriterDescriptor; this file is the
// definition.
//
// The semantics below were settled by probing TagLib 2.3.2 against real
// fixtures of every container it opens, not by reading its docs:
//
//   * File::setProperties() replaces the whole tag: keys absent from the map
//     are dropped. A merge write therefore reads file.properties() first and
//     lays the new values over it.
//   * setProperties() alone leaves an existing picture block untouched, in
//     either direction. Deleting art takes an explicit empty PICTURE list
//     through setComplexProperties(); adding it takes a one-element list.
//     That call works on files with no tags at all, and works before or
//     after setProperties; art-first order is what the probes ran.
//   * Multi-value StringLists survive a save in every container probed, and
//     arbitrary keys survive in Vorbis-comment and APEv2 containers; ID3v2
//     folds unknown names into TXX text frames (the read-back key comes back
//     uppercased and folded), and MP4 parks them in vendor-namespaced free
//     atoms under names MP4 has no item for. Those report lossyRepresentation.
//   * MPEG, WAVE, DSDIFF and friends merge the tag types they hold on read:
//     properties() hands back ID3v2 fused with APE and ID3v1. A rewrite that
//     only replaced the ID3v2 block would read back the stale ID3v1 or APE as
//     if it were the new tag, so those families save with StripOthers. MPEG's
//     plain save() also mints an ID3v1 duplicate of the ID3v2 it just wrote;
//     DoNotDuplicate stops that.
//   * TagLib advertises save() on Shorten and on the module formats but their
//     own headers say saving tags is not supported; both report unwritable.

#include "xpcog/core/PluginRegistry.hpp"

#include <aifffile.h>
#include <apefile.h>
#include <dsdifffile.h>
#include <dsffile.h>
#include <fileref.h>
#include <flacfile.h>
#include <id3v2.h>
#include <modfilebase.h>
#include <mp4file.h>
#include <mpegfile.h>
#include <oggfile.h>
#include <oggflacfile.h>
#include <opusfile.h>
#include <shortenfile.h>
#include <speexfile.h>
#include <tdebuglistener.h>
#include <tfile.h>
#include <tpropertymap.h>
#include <tvariant.h>
#include <vorbisfile.h>
#include <wavfile.h>
#include <wavpackfile.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

[[nodiscard]] std::string toUtf8(const TagLib::String& text) {
    return text.to8Bit(/*unicode=*/true);
}

[[nodiscard]] std::string lowercased(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return text;
}

/// TagLib writes complaints straight to stderr by default; the reader keeps
/// them off a scan and here they explain a failed save, so each entry point
/// installs a listener for its own duration and restores this one afterwards.
/// TagLib has no getter for the current listener. The reader's translation
/// unit has its own silent listener and never restores it, so the last write
/// to finish leaves this one installed; both swallow, which is the invariant
/// that matters.
class SilentDebugListener : public TagLib::DebugListener {
public:
    void printMessage(const TagLib::String&) override {}
};

SilentDebugListener& silentListener() {
    static SilentDebugListener listener;
    return listener;
}

/// Scoped listener collecting TagLib's messages for one write, to ride back
/// in MetadataWriteOptions::error.
class RecordingDebugListener : public TagLib::DebugListener {
public:
    void printMessage(const TagLib::String& message) override {
        if (!collected.empty()) {
            collected += "; ";
        }
        collected += toUtf8(message);
    }

    std::string collected;
};

class DebugListenerScope {
public:
    explicit DebugListenerScope(TagLib::DebugListener& listener) {
        TagLib::setDebugListener(&listener);
    }

    ~DebugListenerScope() {
        TagLib::setDebugListener(&silentListener());
    }

    DebugListenerScope(const DebugListenerScope&)            = delete;
    DebugListenerScope& operator=(const DebugListenerScope&) = delete;
};

[[nodiscard]] TagLib::ByteVector toByteVector(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()),
            static_cast<TagLib::offset_t>(bytes.size())};
}

/// Magic-number sniff for the mime label that goes into the picture block.
/// The fallback matters for FLAC, whose spec treats an empty label as a link
/// to external art; the rest ignore the label or drop it (MP4's covr atoms
/// and APE's cover art items keep the bytes only).
[[nodiscard]] std::string sniffMimeType(const std::vector<std::byte>& bytes) {
    auto startsWith = [&](std::initializer_list<unsigned char> magic) {
        if (bytes.size() < magic.size()) {
            return false;
        }
        return std::equal(magic.begin(), magic.end(), bytes.begin(), [](unsigned char a, std::byte b) {
            return a == static_cast<unsigned char>(b);
        });
    };
    if (startsWith({0x89, 0x50, 0x4E, 0x47})) {
        return "image/png";
    }
    if (startsWith({0xFF, 0xD8, 0xFF})) {
        return "image/jpeg";
    }
    if (startsWith({'G', 'I', 'F'})) {
        return "image/gif";
    }
    if (bytes.size() >= 12 && startsWith({'R', 'I', 'F', 'F'})
        && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
        return "image/webp";
    }
    if (startsWith({'B', 'M'})) {
        return "image/bmp";
    }
    return "image/png";
}

/// Which container TagLib actually opened, by its own parser rather than by
/// the extension.
enum class Family {
    vorbis,      ///< FLAC (native and in Ogg) and the Ogg audio codecs
    id3v2,       ///< MP3, WAV, AIFF, DSF, DSDIFF
    mp4,
    apeTagged,   ///< Monkey's Audio and WavPack: APEv2 items
    conservative, ///< TagLib opens it and can save it, but makes no promises:
                  ///< Matroska, MPC, ASF, TrueAudio, anything newer
    unwritable,   ///< Shorten and the module formats, per TagLib's own headers
};

[[nodiscard]] Family familyOf(const TagLib::FileRef& file) {
    TagLib::File* raw = file.file();
    if (dynamic_cast<TagLib::FLAC::File*>(raw) || dynamic_cast<TagLib::Ogg::File*>(raw)) {
        // Ogg::File covers Vorbis, Opus, Speex and Ogg-FLAC; all carry Vorbis
        // comments and all save through the same xiph-comment path.
        return Family::vorbis;
    }
    if (dynamic_cast<TagLib::MPEG::File*>(raw) || dynamic_cast<TagLib::RIFF::WAV::File*>(raw)
        || dynamic_cast<TagLib::RIFF::AIFF::File*>(raw) || dynamic_cast<TagLib::DSF::File*>(raw)
        || dynamic_cast<TagLib::DSDIFF::File*>(raw)) {
        return Family::id3v2;
    }
    if (dynamic_cast<TagLib::MP4::File*>(raw)) {
        return Family::mp4;
    }
    if (dynamic_cast<TagLib::APE::File*>(raw) || dynamic_cast<TagLib::WavPack::File*>(raw)) {
        return Family::apeTagged;
    }
    if (dynamic_cast<TagLib::Shorten::File*>(raw) || dynamic_cast<TagLib::Mod::FileBase*>(raw)) {
        return Family::unwritable;
    }
    return Family::conservative;
}

[[nodiscard]] MetadataCapabilities capabilitiesFor(Family family) {
    MetadataCapabilities caps;
    switch (family) {
    case Family::vorbis:
        caps.writable           = true;
        caps.arbitraryKeys      = true;
        caps.multiValue         = true;
        caps.pictures           = true;
        break;
    case Family::id3v2:
        // Arbitrary names ride TXX text frames, with the name folded into the
        // frame's value: the round-tripped key can come back different.
        caps.writable            = true;
        caps.multiValue          = true;
        caps.pictures            = true;
        caps.lossyRepresentation = true;
        break;
    case Family::mp4:
        // Arbitrary names survive in vendor-namespaced free-form atoms under
        // names MP4 has no standard item for, and picture blocks keep no
        // description or picture-type label.
        caps.writable            = true;
        caps.multiValue          = true;
        caps.pictures            = true;
        caps.lossyRepresentation = true;
        break;
    case Family::apeTagged:
        // APEv2 items take arbitrary names and repeat values. Picture bytes
        // round-trip through the cover-art items, though the mime label is
        // lost -- and albumart is opaque bytes in XPCog, so nothing reads it
        // back anyway.
        caps.writable      = true;
        caps.arbitraryKeys = true;
        caps.multiValue    = true;
        caps.pictures      = true;
        break;
    case Family::conservative:
        // Probed nothing about these; say only that TagLib can rewrite the
        // file. The editor falls back to full replace with no promises.
        caps.writable            = true;
        caps.lossyRepresentation = true;
        break;
    case Family::unwritable:
        break;
    }
    return caps;
}

/// Container-specific save. Plain save() would do for most families, but the
/// multi-tag ones need explicit arguments: the version request only reaches
/// the containers that carry ID3v2, and StripOthers keeps a stale ID3v1 or
/// APE companion from reading back as the new tag (see file header). MPEG's
/// DoNotDuplicate stops its default save from duplicating the fresh ID3v2
/// into an ID3v1 block nobody asked for.
bool saveFile(TagLib::FileRef& file, const MetadataWriteOptions& options) {
    const TagLib::ID3v2::Version version =
        options.id3v2Version == 3 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4;
    TagLib::File* raw = file.file();

    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(raw)) {
        return mpeg->save(TagLib::MPEG::File::AllTags, TagLib::File::StripOthers, version,
                          TagLib::File::DoNotDuplicate);
    }
    if (auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(raw)) {
        return wav->save(TagLib::RIFF::WAV::File::AllTags, TagLib::File::StripOthers, version);
    }
    if (auto* dsdiff = dynamic_cast<TagLib::DSDIFF::File*>(raw)) {
        return dsdiff->save(TagLib::DSDIFF::File::AllTags, TagLib::File::StripOthers, version);
    }
    if (auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(raw)) {
        return aiff->save(version);
    }
    if (auto* dsf = dynamic_cast<TagLib::DSF::File*>(raw)) {
        return dsf->save(version);
    }
    return file.save();
}

/// PropertyMap keys are case-sensitive, but every TagLib tag implementation
/// reads back with the key uppercased. In merge mode the base map therefore
/// carries "ARTIST" while the edit names it "artist": laying the new value in
/// without removing the other spelling writes the tag twice. Erase matches
/// case-insensitively for exactly that reason.
void eraseCaseInsensitively(TagLib::PropertyMap& properties, const std::string& key) {
    const std::string lowered = lowercased(key);
    std::vector<TagLib::String> doomed;
    for (const auto& [name, values] : properties) {
        static_cast<void>(values);
        if (lowercased(toUtf8(name)) == lowered) {
            doomed.push_back(name);
        }
    }
    for (const auto& name : doomed) {
        properties.erase(name);
    }
}

}  // namespace

MetadataCapabilities probeTagsWithTagLib(const Url& url) {
    const auto path = url.localPath();
    if (!path) {
        return {};
    }

    DebugListenerScope silence{silentListener()};

    TagLib::FileRef file{TagLib::FileName{path->c_str()}, /*readAudioProperties=*/false};
    if (file.isNull() || file.file() == nullptr) {
        return {};
    }
    return capabilitiesFor(familyOf(file));
}

bool writeTagsWithTagLib(const Url& url, const MetadataMap& tags, MetadataWriteOptions& options) {
    const auto path = url.localPath();
    if (!path) {
        // Same stance as the reader: TagLib opens and rewrites the container
        // itself, so it needs a real file.
        options.error = "tags can only be written to a file on disk";
        return false;
    }

    RecordingDebugListener recorder;
    DebugListenerScope       scope{recorder};

    TagLib::FileRef file{TagLib::FileName{path->c_str()}, /*readAudioProperties=*/false};
    if (file.isNull() || file.file() == nullptr) {
        options.error = "the file could not be opened for writing";
        return false;
    }

    const Family family = familyOf(file);
    if (family == Family::unwritable) {
        options.error = "this file format does not support saving tags";
        return false;
    }
    const MetadataCapabilities caps = capabilitiesFor(family);

    // Replace is the default (see MetadataWriteOptions): the map an editor
    // hands over came from a scan of this same file, so keys it does not
    // carry were deleted on purpose. Merging starts from what is in there.
    TagLib::PropertyMap properties;
    if (!options.stripExisting) {
        properties = file.properties();
    }

    // Album art is the bytes value; every family that keeps pictures stores
    // it through the same complex "PICTURE" property TagLib reads it from.
    // One image per file, matching what the reader keeps.
    const std::vector<std::byte>* artwork = nullptr;
    bool                          artCleared = false;
    for (const auto& entry : tags) {
        if (lowercased(entry.key) != "albumart" || !std::holds_alternative<std::vector<std::byte>>(entry.value)) {
            continue;
        }
        const auto& bytes = std::get<std::vector<std::byte>>(entry.value);
        if (bytes.empty()) {
            artCleared = true;
        } else {
            artwork = &bytes;
        }
        break;
    }
    if (caps.pictures) {
        if (artwork != nullptr) {
            TagLib::VariantMap picture;
            picture["data"]        = toByteVector(*artwork);
            picture["mimeType"]    = TagLib::String(sniffMimeType(*artwork));
            picture["description"] = TagLib::String();
            picture["pictureType"] = TagLib::String("Front Cover");
            TagLib::List<TagLib::VariantMap> pictures;
            pictures.append(picture);
            file.setComplexProperties("PICTURE", pictures);
        } else if (artCleared || options.stripExisting) {
            // setProperties does not disturb an existing picture block in
            // either direction, so removing art -- by an explicit empty
            // albumart, or because a replace write carried none -- takes the
            // empty list.
            file.setComplexProperties("PICTURE", TagLib::List<TagLib::VariantMap>());
        }
    }
    // Families without caps.pictures get the bytes dropped: the editor is
    // not shown the art row for them, so this only guards a blind write.

    for (const auto& entry : tags) {
        const std::string key = lowercased(entry.key);
        const auto*       values = std::get_if<std::vector<std::string>>(&entry.value);
        if (values == nullptr) {
            continue;  // albumart rode out above; any other bytes have nowhere to go
        }
        if (values->empty()) {
            // An empty value list means "delete this tag". It must not reach
            // setProperties: under ID3v2 an empty StringList still writes a
            // frame, which reads back as a tag with a name and no value.
            eraseCaseInsensitively(properties, key);
            continue;
        }
        eraseCaseInsensitively(properties, key);
        TagLib::StringList list;
        for (const auto& value : *values) {
            list.append(TagLib::String(value, TagLib::String::UTF8));
        }
        properties[TagLib::String(key)] = list;
    }

    // TagLib's generic property path has nowhere to put a name the container
    // has no frame for; the capability flags say up front which containers
    // fold and which keep anything, so an unsupported-key report here is a
    // format we probed nothing about, and there is nothing to do with it
    // beyond letting the tags that did land land.
    file.setProperties(properties);

    if (!saveFile(file, options)) {
        options.error =
            recorder.collected.empty() ? "the file could not be saved" : "the file could not be saved: " + recorder.collected;
        return false;
    }
    return true;
}

}  // namespace xpcog
