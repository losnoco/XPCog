#include "xpcog/core/library/PlaylistFile.hpp"

#include "xpcog/core/FilePath.hpp"

#include "PropertyList.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace xpcog {
namespace {

[[nodiscard]] std::string lowercased(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

[[nodiscard]] std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t              start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        // Leading and trailing space, but not interior: a path may contain it.
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            lines.emplace_back(line);
        }
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

/// The directory a playlist's relative paths are relative to, with a trailing
/// separator. Empty when the playlist is not local.
[[nodiscard]] std::string baseDirectoryOf(const Url& playlist) {
    const auto path = playlist.localPath();
    if (!path) {
        return {};
    }
    std::string directory = pathToUtf8Generic(path->parent_path());
    if (!directory.empty() && directory.back() != '/') {
        directory.push_back('/');
    }
    return directory;
}

/// Resolves one stored path. Port of XmlContainer.m +urlForPath:relativeTo:,
/// which is also what the M3U and PLS readers need.
[[nodiscard]] Url resolveStoredPath(std::string_view stored, const Url& playlist) {
    if (stored.find("://") != std::string_view::npos) {
        if (const auto parsed = Url::parse(stored)) {
            return *parsed;
        }
    }

    std::string path{stored};

    // A trailing "#digits" is a cue-sheet track or subsong index, not part of
    // the file name. Cog scans for exactly that shape, so a file genuinely
    // called "Track #1.flac" survives.
    std::string fragment;
    const std::size_t hash = path.rfind('#');
    if (hash != std::string::npos && hash + 1 < path.size()) {
        const std::string_view candidate{path.data() + hash + 1, path.size() - hash - 1};
        if (std::all_of(candidate.begin(), candidate.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
            fragment = candidate;
            path.resize(hash);
        }
    }

    const bool absolute = path.starts_with('/') ||
                          (path.size() > 2 && path[1] == ':');  // C:\ on Windows
    if (!absolute) {
        // Only relative paths can carry Windows separators, because an absolute
        // one came from a URL. Cog reasons the same way.
        std::replace(path.begin(), path.end(), '\\', '/');
        path.insert(0, baseDirectoryOf(playlist));
    }

    Url url = Url::fromLocalPath(pathFromUtf8(path));
    return fragment.empty() ? url : url.withFragment(fragment);
}

// --- Cog XML keys -------------------------------------------------------

/// Cog builds its item dictionaries by runtime introspection of PlaylistEntry
/// (PlaylistLoader.m:163), so the keys are Core Data property names. These are
/// the ones that carry information a reader needs.
constexpr std::string_view kKeyUrl            = "URL";
constexpr std::string_view kKeyAlbumArt       = "albumArt";
constexpr std::string_view kKeyMetadataBlob   = "metadataBlob";

void applyCogXmlProperties(const plist::Value& item, PlaylistEntry& entry) {
    TrackProperties& properties = entry.properties;

    properties.format.sampleRate    = item.realValue("sampleRate");
    properties.format.channels =
        static_cast<std::uint32_t>(item.integerValue("channels"));
    properties.format.channelConfig =
        static_cast<std::uint32_t>(item.integerValue("channelConfig"));
    properties.format.bitsPerSample =
        static_cast<std::uint32_t>(item.integerValue("bitsPerSample"));
    properties.format.bigEndian = lowercased(item.stringValue("endian")) == "big";

    properties.totalFrames = item.integerValue("totalFrames");
    properties.bitrateKbps = static_cast<std::int32_t>(item.integerValue("bitrate"));
    properties.seekable    = item.boolValue("seekable");
    properties.codec       = item.stringValue("codec");
    properties.encoding    = item.stringValue("encoding");
    properties.lossless    = properties.encoding == "lossless";
    if (const plist::Value* cuesheet = item.find("cuesheet");
        cuesheet != nullptr && !cuesheet->string.empty()) {
        properties.cuesheet = cuesheet->string;
    }

    // Cog stores these as scalar floats defaulting to 0, so it cannot tell "no
    // album gain" from "0 dB". Treating an absent key as absent is the only
    // reading that does not silently apply 0 dB to everything.
    const auto readGain = [&item](std::string_view key) -> std::optional<float> {
        const plist::Value* value = item.find(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        return static_cast<float>(item.realValue(key));
    };
    ReplayGainInfo& gain = properties.replayGain;
    gain.trackGain       = readGain("replayGainTrackGain");
    gain.trackPeak       = readGain("replayGainTrackPeak");
    gain.albumGain       = readGain("replayGainAlbumGain");
    gain.albumPeak       = readGain("replayGainAlbumPeak");
    gain.volume          = readGain("volume");
    if (const plist::Value* soundcheck = item.find("soundcheck");
        soundcheck != nullptr && !soundcheck->string.empty()) {
        gain.soundcheck = soundcheck->string;
    }

    entry.metadataLoaded = item.boolValue("metadataLoaded");
    entry.playCount      = item.integerValue("playCount");
}

[[nodiscard]] MetadataMap tagsFromCogXml(const plist::Value& item) {
    MetadataMap tags;

    // metadataBlob is the authority: Cog's own album/artist/title accessors read
    // it, and the top-level keys are just its introspected getters written out.
    if (const plist::Value* blob = item.find(kKeyMetadataBlob);
        blob != nullptr && blob->type == plist::Value::Type::Dict) {
        for (const auto& [key, value] : blob->dict) {
            if (value.type == plist::Value::Type::Array) {
                for (const plist::Value& element : value.array) {
                    tags.add(key, element.string);
                }
            } else if (value.type == plist::Value::Type::String) {
                tags.add(key, value.string);
            } else if (value.type == plist::Value::Type::Data) {
                tags.setBytes(key, value.data);
            }
        }
    }

    // Fall back to the flattened keys for anything the blob did not carry, so a
    // playlist written by a tool that only emitted those still reads.
    const auto fallback = [&item, &tags](std::string_view tagName,
                                         std::string_view itemKey) {
        if (tags.contains(tagName)) {
            return;
        }
        const std::string value = item.stringValue(itemKey);
        if (!value.empty()) {
            tags.set(tagName, value);
        }
    };
    fallback("album", "album");
    fallback("albumartist", "albumartist");
    fallback("artist", "artist");
    fallback("title", "rawTitle");
    fallback("title", "title");
    fallback("genre", "genre");
    fallback("composer", "composer");
    fallback("date", "date");
    fallback("comment", "comment");
    fallback("unsyncedlyrics", "unsyncedlyrics");
    fallback("tracknumber", "track");
    fallback("discnumber", "disc");

    return tags;
}

/// The tag map to write back out, with the promoted columns folded in under the
/// names Cog's own reader looks for.
[[nodiscard]] MetadataMap tagsForCogXml(const PlaylistEntry& entry) {
    MetadataMap tags;
    const auto  put = [&tags](std::string_view key, const std::string& value) {
        if (!value.empty()) {
            tags.set(key, value);
        }
    };
    put("album", entry.album);
    put("albumartist", entry.albumArtist);
    put("artist", entry.artist);
    put("title", entry.rawTitle);
    put("genre", entry.genre);
    put("composer", entry.composer);
    put("date", entry.date);
    put("comment", entry.comment);
    put("unsyncedlyrics", entry.unsyncedLyrics);
    if (entry.track != 0) {
        tags.set("tracknumber", std::to_string(entry.track));
    }
    if (entry.disc != 0) {
        tags.set("discnumber", std::to_string(entry.disc));
    }
    tags.mergeFrom(entry.metadata);
    return tags;
}

// --- XSPF ---------------------------------------------------------------

/// Text of the first `<name>` inside `[begin, end)`, entity-decoded.
///
/// A scan rather than a parse. XSPF in the wild is machine-generated and flat,
/// and a second full XML parser to read six leaf elements would be more code
/// than it is worth. Namespace prefixes on element names are not handled.
[[nodiscard]] std::string xspfElement(std::string_view region, std::string_view name) {
    const std::string open  = "<" + std::string{name};
    const std::string close = "</" + std::string{name} + ">";

    const std::size_t start = region.find(open);
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t contentStart = region.find('>', start);
    if (contentStart == std::string_view::npos) {
        return {};
    }
    // Self-closing: <title/>
    if (region[contentStart - 1] == '/') {
        return {};
    }
    const std::size_t end = region.find(close, contentStart);
    if (end == std::string_view::npos) {
        return {};
    }

    std::string_view text = region.substr(contentStart + 1, end - contentStart - 1);
    if (text.starts_with("<![CDATA[") && text.ends_with("]]>")) {
        text.remove_prefix(9);
        text.remove_suffix(3);
        return std::string{text};
    }
    return plist::xmlDecodeEntities(text);
}

// --- readers ------------------------------------------------------------

[[nodiscard]] PlaylistEntry entryForUrl(Url url) {
    PlaylistEntry entry;
    entry.url = std::move(url);
    return entry;
}

[[nodiscard]] PlaylistFileContents readM3u(std::string_view text, const Url& source) {
    PlaylistFileContents contents;
    for (const std::string& line : splitLines(text)) {
        if (line.starts_with('#')) {
            continue;  // #EXTINF and friends carry only display metadata
        }
        contents.entries.push_back(entryForUrl(resolveStoredPath(line, source)));
    }
    return contents;
}

[[nodiscard]] PlaylistFileContents readPls(std::string_view text, const Url& source) {
    std::vector<std::pair<long, Url>> numbered;

    for (const std::string& line : splitLines(text)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = lowercased(std::string_view{line}.substr(0, equals));
        if (!key.starts_with("file")) {
            continue;
        }
        const std::string_view index{key.data() + 4, key.size() - 4};
        if (index.empty() || !std::all_of(index.begin(), index.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            continue;
        }
        const std::string value = line.substr(equals + 1);
        if (value.empty()) {
            continue;
        }
        numbered.emplace_back(std::strtol(std::string{index}.c_str(), nullptr, 10),
                              resolveStoredPath(value, source));
    }

    // FileN numbering gives the order, which need not match line order.
    std::stable_sort(numbered.begin(), numbered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    PlaylistFileContents contents;
    contents.entries.reserve(numbered.size());
    for (auto& [number, url] : numbered) {
        contents.entries.push_back(entryForUrl(std::move(url)));
    }
    return contents;
}

[[nodiscard]] PlaylistFileContents readXspf(std::string_view text, const Url& source) {
    PlaylistFileContents contents;

    std::size_t position = 0;
    for (;;) {
        const std::size_t start = text.find("<track", position);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t end = text.find("</track>", start);
        if (end == std::string_view::npos) {
            break;
        }
        const std::string_view region = text.substr(start, end - start);
        position                      = end + 8;

        const std::string location = xspfElement(region, "location");
        if (location.empty()) {
            continue;
        }

        PlaylistEntry entry;
        entry.url      = resolveStoredPath(percentDecode(location), source);
        entry.rawTitle = xspfElement(region, "title");
        entry.artist   = xspfElement(region, "creator");
        entry.album    = xspfElement(region, "album");
        entry.comment  = xspfElement(region, "annotation");

        const std::string track = xspfElement(region, "trackNum");
        if (!track.empty()) {
            entry.track = static_cast<std::int32_t>(std::strtol(track.c_str(), nullptr, 10));
        }

        // XSPF durations are milliseconds. Without a sample rate there is no
        // frame count, so 44100 is assumed purely so the duration displays --
        // a rescan replaces both with the truth.
        const std::string duration = xspfElement(region, "duration");
        if (!duration.empty()) {
            const long milliseconds = std::strtol(duration.c_str(), nullptr, 10);
            if (milliseconds > 0) {
                entry.properties.format.sampleRate = 44100.0;
                entry.properties.totalFrames =
                    static_cast<std::int64_t>(milliseconds) * 44100 / 1000;
            }
        }

        contents.entries.push_back(std::move(entry));
    }
    return contents;
}

[[nodiscard]] std::optional<PlaylistFileContents> readCogXml(std::string_view text,
                                                             const Url&       source) {
    const auto root = plist::parse(text);
    if (!root) {
        return std::nullopt;
    }

    // A bare array of items is the older shape; the dict adds artwork and queue.
    const plist::Value* items    = nullptr;
    const plist::Value* artwork  = nullptr;
    const plist::Value* queue    = nullptr;
    if (root->type == plist::Value::Type::Array) {
        items = &*root;
    } else if (root->type == plist::Value::Type::Dict) {
        items   = root->find("items");
        artwork = root->find(kKeyAlbumArt);
        queue   = root->find("queue");
    }
    if (items == nullptr || items->type != plist::Value::Type::Array) {
        return std::nullopt;
    }

    PlaylistFileContents contents;
    for (const plist::Value& item : items->array) {
        if (item.type != plist::Value::Type::Dict) {
            continue;
        }

        PlaylistEntry entry;
        entry.url = resolveStoredPath(item.stringValue(kKeyUrl), source);
        entry.applyMetadata(tagsFromCogXml(item));
        applyCogXmlProperties(item, entry);

        // Cog keys artwork by MD5 of the image; XPCog stores it by SHA-256. The
        // stored key is treated as opaque here and the bytes are re-hashed by
        // the Library, so the two never have to agree.
        const std::string artKey = item.stringValue(kKeyAlbumArt);
        if (!artKey.empty() && artwork != nullptr) {
            if (const plist::Value* image = artwork->find(artKey);
                image != nullptr && image->type == plist::Value::Type::Data) {
                entry.artHash = artKey;
                const bool known =
                    std::any_of(contents.artwork.begin(), contents.artwork.end(),
                                [&artKey](const auto& pair) { return pair.first == artKey; });
                if (!known) {
                    contents.artwork.emplace_back(artKey, image->data);
                }
            }
        }

        contents.entries.push_back(std::move(entry));
    }

    if (queue != nullptr && queue->type == plist::Value::Type::Array) {
        for (const plist::Value& index : queue->array) {
            const auto position = static_cast<std::size_t>(index.integer);
            if (index.type == plist::Value::Type::Integer &&
                position < contents.entries.size()) {
                contents.queue.push_back(position);
            }
        }
    }

    return contents;
}

// --- writers ------------------------------------------------------------

[[nodiscard]] std::string writeM3u(const std::vector<PlaylistEntry>& entries,
                                   const Url&                        destination) {
    // Cog opens with a bare "#" (PlaylistLoader.m:126). Kept so a diff against a
    // Cog-written file is empty rather than one line off.
    std::string out = "#\n";
    for (const PlaylistEntry& entry : entries) {
        out += relativePathFor(entry.url, destination);
        out += '\n';
    }
    return out;
}

[[nodiscard]] std::string writePls(const std::vector<PlaylistEntry>& entries,
                                   const Url&                        destination) {
    std::string out = "[playlist]\nnumberOfEntries=" + std::to_string(entries.size()) +
                      "\n\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        out += "File" + std::to_string(i + 1) + "=" +
               relativePathFor(entries[i].url, destination) + "\n";
    }
    out += "\nVERSION=2";
    return out;
}

[[nodiscard]] std::string writeXspf(const std::vector<PlaylistEntry>& entries,
                                    const Url&                        destination) {
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\">\n"
        "\t<trackList>\n";

    const auto element = [&out](std::string_view name, const std::string& value) {
        if (value.empty()) {
            return;
        }
        out += "\t\t\t<" + std::string{name} + ">" + plist::xmlEscape(value) + "</" +
               std::string{name} + ">\n";
    };

    for (const PlaylistEntry& entry : entries) {
        out += "\t\t<track>\n";
        element("location", percentEncodePath(relativePathFor(entry.url, destination)));
        element("title", entry.rawTitle);
        element("creator", entry.artist);
        element("album", entry.album);
        element("annotation", entry.comment);
        if (entry.track != 0) {
            element("trackNum", std::to_string(entry.track));
        }
        const double seconds = entry.duration();
        if (seconds > 0.0) {
            element("duration", std::to_string(static_cast<std::int64_t>(seconds * 1000.0)));
        }
        out += "\t\t</track>\n";
    }

    out += "\t</trackList>\n</playlist>\n";
    return out;
}

[[nodiscard]] std::string writeCogXml(const std::vector<PlaylistEntry>& entries,
                                      const std::vector<std::size_t>&   queue,
                                      const Url&                        destination,
                                      ArtworkLookup                     artworkFor,
                                      void*                             context) {
    plist::Value artwork;
    artwork.type = plist::Value::Type::Dict;

    plist::Value items;
    items.type = plist::Value::Type::Array;

    for (const PlaylistEntry& entry : entries) {
        std::vector<std::pair<std::string, plist::Value>> fields;
        const auto put = [&fields](std::string_view key, plist::Value value) {
            fields.emplace_back(std::string{key}, std::move(value));
        };
        const auto putString = [&put](std::string_view key, const std::string& value) {
            if (!value.empty()) {
                put(key, plist::Value::ofString(value));
            }
        };

        putString(kKeyUrl, relativePathFor(entry.url, destination));
        putString("album", entry.album);
        putString("albumartist", entry.albumArtist);
        putString("artist", entry.artist);
        putString("rawTitle", entry.rawTitle);
        putString("title", entry.title());
        putString("genre", entry.genre);
        putString("composer", entry.composer);
        putString("date", entry.date);
        putString("comment", entry.comment);
        putString("unsyncedlyrics", entry.unsyncedLyrics);
        if (entry.track != 0) {
            put("track", plist::Value::ofInteger(entry.track));
        }
        if (entry.disc != 0) {
            put("disc", plist::Value::ofInteger(entry.disc));
        }

        const TrackProperties& properties = entry.properties;
        if (properties.format.sampleRate > 0.0) {
            put("sampleRate", plist::Value::ofReal(properties.format.sampleRate));
        }
        if (properties.format.channels != 0) {
            put("channels", plist::Value::ofInteger(properties.format.channels));
        }
        if (properties.format.channelConfig != 0) {
            put("channelConfig", plist::Value::ofInteger(properties.format.channelConfig));
        }
        if (properties.format.bitsPerSample != 0) {
            put("bitsPerSample", plist::Value::ofInteger(properties.format.bitsPerSample));
        }
        if (properties.format.bigEndian) {
            put("endian", plist::Value::ofString("big"));
        }
        if (properties.totalFrames != 0) {
            put("totalFrames", plist::Value::ofInteger(properties.totalFrames));
        }
        if (properties.bitrateKbps != 0) {
            put("bitrate", plist::Value::ofInteger(properties.bitrateKbps));
        }
        put("seekable", plist::Value::ofBool(properties.seekable));
        putString("codec", properties.codec);
        putString("encoding", properties.encoding);
        if (properties.cuesheet) {
            putString("cuesheet", *properties.cuesheet);
        }

        const ReplayGainInfo& gain = properties.replayGain;
        const auto putGain = [&put](std::string_view key, const std::optional<float>& value) {
            if (value) {
                put(key, plist::Value::ofReal(static_cast<double>(*value)));
            }
        };
        putGain("replayGainTrackGain", gain.trackGain);
        putGain("replayGainTrackPeak", gain.trackPeak);
        putGain("replayGainAlbumGain", gain.albumGain);
        putGain("replayGainAlbumPeak", gain.albumPeak);
        putGain("volume", gain.volume);
        if (gain.soundcheck) {
            putString("soundcheck", *gain.soundcheck);
        }

        // Cog omits metadataLoaded on an errored entry so that reopening retries
        // the file rather than trusting a failed read (PlaylistLoader.m:227).
        if (!entry.error) {
            put("metadataLoaded", plist::Value::ofBool(entry.metadataLoaded));
        }
        if (entry.playCount != 0) {
            put("playCount", plist::Value::ofInteger(entry.playCount));
        }

        if (!entry.artHash.empty() && artworkFor != nullptr) {
            if (artwork.find(entry.artHash) == nullptr) {
                std::vector<std::byte> image = artworkFor(entry.artHash, context);
                if (!image.empty()) {
                    artwork.dict.emplace_back(entry.artHash,
                                              plist::Value::ofData(std::move(image)));
                }
            }
            if (artwork.find(entry.artHash) != nullptr) {
                putString(kKeyAlbumArt, entry.artHash);
            }
        }

        // The tag map goes in whole, because Cog's own album/artist accessors
        // read metadataBlob rather than the flattened keys above.
        plist::Value blob;
        blob.type = plist::Value::Type::Dict;
        for (const auto& [key, value] : tagsForCogXml(entry)) {
            if (const auto* strings = std::get_if<std::vector<std::string>>(&value)) {
                std::vector<plist::Value> values;
                values.reserve(strings->size());
                for (const std::string& text : *strings) {
                    values.push_back(plist::Value::ofString(text));
                }
                blob.dict.emplace_back(key, plist::Value::ofArray(std::move(values)));
            } else if (const auto* bytes = std::get_if<std::vector<std::byte>>(&value)) {
                blob.dict.emplace_back(key, plist::Value::ofData(*bytes));
            }
        }
        if (!blob.dict.empty()) {
            put(kKeyMetadataBlob, std::move(blob));
        }

        items.array.push_back(plist::Value::ofDict(std::move(fields)));
    }

    plist::Value queueValue;
    queueValue.type = plist::Value::Type::Array;
    for (const std::size_t index : queue) {
        queueValue.array.push_back(plist::Value::ofInteger(static_cast<std::int64_t>(index)));
    }

    return plist::write(plist::Value::ofDict({
        {std::string{kKeyAlbumArt}, std::move(artwork)},
        {"items", std::move(items)},
        {"queue", std::move(queueValue)},
    }));
}

}  // namespace

// --- public interface ---------------------------------------------------

std::optional<PlaylistFormat> playlistFormatForExtension(std::string_view extension) {
    const std::string lower = lowercased(extension);
    if (lower == "m3u" || lower == "m3u8") {
        return PlaylistFormat::M3u;
    }
    if (lower == "pls") {
        return PlaylistFormat::Pls;
    }
    if (lower == "xspf") {
        return PlaylistFormat::Xspf;
    }
    if (lower == "xml") {
        return PlaylistFormat::CogXml;
    }
    return std::nullopt;
}

std::string relativePathFor(const Url& entry, const Url& destination) {
    const auto entryPath = entry.localPath();
    if (!entryPath) {
        return entry.toString();  // remote entries stay absolute
    }

    std::string path = pathToUtf8Generic(*entryPath);

    // Cog strips an exact, case-insensitive directory prefix and nothing more
    // (PlaylistLoader.m:98) -- it never walks up with "..". Reproduced: a
    // playlist beside its music becomes relative, one elsewhere stays absolute,
    // and neither can accidentally point outside the tree it was written in.
    const std::string base = baseDirectoryOf(destination);
    if (!base.empty() && path.size() > base.size()) {
        const bool matches = std::equal(
            base.begin(), base.end(), path.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            });
        if (matches) {
            path.erase(0, base.size());
        }
    }

    const std::string_view fragment = entry.fragment();
    if (!fragment.empty()) {
        path += '#';
        path += fragment;
    }
    return path;
}

std::optional<PlaylistFileContents> readPlaylist(PlaylistFormat   format,
                                                 std::string_view text,
                                                 const Url&       source) {
    switch (format) {
        case PlaylistFormat::M3u: return readM3u(text, source);
        case PlaylistFormat::Pls: return readPls(text, source);
        case PlaylistFormat::Xspf: return readXspf(text, source);
        case PlaylistFormat::CogXml: return readCogXml(text, source);
    }
    return std::nullopt;
}

std::string writePlaylist(PlaylistFormat format, const std::vector<PlaylistEntry>& entries,
                          const std::vector<std::size_t>& queue, const Url& destination,
                          ArtworkLookup artworkFor, void* context) {
    switch (format) {
        case PlaylistFormat::M3u: return writeM3u(entries, destination);
        case PlaylistFormat::Pls: return writePls(entries, destination);
        case PlaylistFormat::Xspf: return writeXspf(entries, destination);
        case PlaylistFormat::CogXml:
            return writeCogXml(entries, queue, destination, artworkFor, context);
    }
    return {};
}

}  // namespace xpcog
