// xpcog-doctypes -- what XPCog.app tells macOS it opens.
//
// Writes the CFBundleDocumentTypes and UTImportedTypeDeclarations entries for
// the bundle's Info.plist, from the codec registry, so what the bundle declares
// is exactly what this build decodes. app/CMakeLists.txt runs it after every
// build and app/SpliceDocumentTypes.cmake puts the result into the plist,
// between two marker comments Info.plist.in carries for the purpose.
//
// Why a program rather than a list. The other two platforms settled this in
// opposite directions, each for a reason: `XPCog --register` on Windows reads
// PluginRegistry::allExtensions() at run time because the program can write the
// registry itself, and packaging/linux writes its MIME list at configure time
// because a .desktop file wants MIME types, of which the codecs claim a handful
// with agreed names. A bundle wants extensions -- around nine hundred, most of
// them vgmstream's, which that library reports from inside itself at run time
// and nowhere a CMake script could read. So the list is made by running the
// registry, at the one point in the build where it exists and the plist has not
// been sealed by a signature yet.
//
// What is written, in three shapes, and why three:
//
//   UTImportedTypeDeclarations -- one type per extension macOS has no type of
//   its own for: co.losno.xpcog.<ext>, conforming to public.audio, or to
//   public.data or public.archive for what a container opens. Imported rather than exported
//   because none of these formats is this project's to own. If anything else on
//   the Mac exports a type for the same extension, LaunchServices takes that
//   one and ignores ours, which is the documented order and the right one.
//
//   CFBundleDocumentTypes, by type -- the types macOS itself declares for these
//   extensions (public.mp3, org.xiph.flac...) and every type imported above,
//   as LSItemContentTypes. The form Apple has asked for since 10.5, and the one
//   that gives a file a Kind in the Finder that says what it is.
//
//   CFBundleDocumentTypes, by extension -- the same extensions once more, as
//   CFBundleTypeExtensions. Deprecated in 10.5, honoured in 26, and it covers
//   the case the entry above cannot: another application exporting a type for
//   .adx (modizer does, for most of vgmstream's list). That type is then the
//   file's, our imported one is not, and the entry by type no longer matches --
//   this one still does, because it names the extension and not the type. A
//   separate entry, since a dictionary carrying LSItemContentTypes has its
//   extensions ignored.
//
//   And a folder, public.folder, with role and rank None -- the pair that
//   accepts a drop on the Dock icon without listing XPCog under "Open With" for
//   every directory on the disk.
//
// LSHandlerRank is Alternate throughout, the rank of a secondary viewer, and
// it is the plist's way of saying what --register says on Windows: XPCog is
// offered, and takes nothing from whatever opened these files before. Default
// on .mp3 is how a freshly installed player finds itself answering every
// double-click on a Mac that had a perfectly good arrangement. Where XPCog is
// the only application declaring a type at all, Alternate still makes it the
// one that opens it, so the game-music formats open by double-click regardless.
//
// The system's own types are asked of LaunchServices here rather than listed,
// by the same rule as the extensions, and that has one consequence worth
// knowing: which extensions macOS has a type for is a fact about the Mac that
// built the bundle. A newer macOS may know one more, in which case a bundle
// built on it names that type where an older build imports its own, and the
// entry by extension covers the difference in either direction. Only types
// declared under /System count. A type some other application exported is not
// one to name in a plist that ships to a Mac that never had the application.
//
// Left out: an extension whose system type is not audiovisual. vgmstream claims
// .m, .l and .r alongside .svg, .ai and .raw, because a game rip can be called
// anything, and Windows offers XPCog for all of them; a Mac knows those names
// as Objective-C source, Illustrator artwork and a camera raw, and an audio
// player in the "Open With" menu of a source file is noise. They are named on
// stderr, so the build log says what was left out and as what.

#include "xpcog/core/PluginRegistry.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

struct CfRelease {
    void operator()(CFTypeRef ref) const {
        if (ref != nullptr) {
            CFRelease(ref);
        }
    }
};

template <class Ref>
using CfPtr = std::unique_ptr<std::remove_pointer_t<Ref>, CfRelease>;

[[nodiscard]] CfPtr<CFStringRef> cfString(std::string_view text) {
    return CfPtr<CFStringRef>{CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
        static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false)};
}

[[nodiscard]] std::string fromCf(CFStringRef text) {
    if (text == nullptr) {
        return {};
    }
    if (const char* direct = CFStringGetCStringPtr(text, kCFStringEncodingUTF8)) {
        return direct;
    }
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(capacity), '\0');
    if (!CFStringGetCString(text, out.data(), capacity, kCFStringEncodingUTF8)) {
        return {};
    }
    out.resize(std::strlen(out.c_str()));
    return out;
}

/// A type macOS declares for an extension, as opposed to one it made up on the
/// spot (`dyn.*`) or one some application on this Mac exported.
struct SystemType {
    std::string identifier;
    std::string description;
    bool        audiovisual = false;  ///< conforms to audiovisual content or a playlist
};

[[nodiscard]] std::optional<SystemType> systemType(std::string_view extension) {
    // Deprecated in macOS 12 in favour of the UniformTypeIdentifiers framework,
    // which is Objective-C and does not say which bundle declared a type -- and
    // that is the one question this asks. The C calls still answer it.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const CfPtr<CFStringRef> tag = cfString(extension);
    const CfPtr<CFStringRef> uti{
        UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, tag.get(), nullptr)};
    if (!uti) {
        return std::nullopt;
    }
    std::string identifier = fromCf(uti.get());
    if (identifier.starts_with("dyn.")) {
        // Nobody declares it; LaunchServices coined this one for the query.
        return std::nullopt;
    }
    const CfPtr<CFURLRef> bundle{UTTypeCopyDeclaringBundleURL(uti.get())};
    if (!bundle) {
        return std::nullopt;
    }
    const CfPtr<CFStringRef> path{CFURLCopyFileSystemPath(bundle.get(), kCFURLPOSIXPathStyle)};
    if (!fromCf(path.get()).starts_with("/System/")) {
        // Some application's, and it may not be on the Mac this ships to.
        return std::nullopt;
    }
    const bool audiovisual =
        UTTypeConformsTo(uti.get(), CFSTR("public.audiovisual-content")) ||
        UTTypeConformsTo(uti.get(), CFSTR("public.playlist"));
    const CfPtr<CFStringRef> description{UTTypeCopyDescription(uti.get())};
    return SystemType{std::move(identifier), fromCf(description.get()), audiovisual};
#pragma clang diagnostic pop
}

/// What a file of an extension is to a Finder user: a track, a list of tracks,
/// or an archive of tracks. A decoder's extension is audio even where a container
/// claims it too, which the cue sheet reader, the HLS decoder and vgmstream's
/// subsong container all do.
enum class Kind { Audio, Playlist, Archive };

[[nodiscard]] const char* conformsTo(Kind kind) {
    switch (kind) {
    case Kind::Audio: return "public.audio";
    case Kind::Archive: return "public.archive";
    case Kind::Playlist: return "public.data";
    }
    return "public.data";
}

[[nodiscard]] const char* noun(Kind kind) {
    switch (kind) {
    case Kind::Audio: return " audio";
    case Kind::Archive: return " archive";
    case Kind::Playlist: return " playlist";
    }
    return "";
}

/// `co.losno.xpcog.<ext>`. A type identifier is letters, digits, hyphens and
/// dots, and a few of vgmstream's extensions carry an underscore.
[[nodiscard]] std::string importedIdentifier(std::string_view extension) {
    std::string id = "co.losno.xpcog.";
    for (const char c : extension) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        id += plain ? c : '-';
    }
    return id;
}

[[nodiscard]] std::string upper(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

[[nodiscard]] std::string escaped(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    }
    return out;
}

void appendUnique(std::vector<std::string>& list, std::string value) {
    for (const std::string& existing : list) {
        if (existing == value) {
            return;
        }
    }
    list.push_back(std::move(value));
}

struct Imported {
    std::string identifier;
    std::string extension;
    Kind        kind;
};

struct LeftOut {
    std::string extension;
    SystemType  type;
};

/// One `<dict>` of CFBundleDocumentTypes.
void writeDocumentType(std::string& out, std::string_view name, std::string_view role,
                       std::string_view rank, std::string_view listKey,
                       const std::vector<std::string>& list) {
    out += "\t\t<dict>\n";
    out += "\t\t\t<key>CFBundleTypeName</key>\n\t\t\t<string>" + escaped(name) + "</string>\n";
    out += "\t\t\t<key>CFBundleTypeRole</key>\n\t\t\t<string>" + std::string{role} + "</string>\n";
    out += "\t\t\t<key>LSHandlerRank</key>\n\t\t\t<string>" + std::string{rank} + "</string>\n";
    out += "\t\t\t<key>" + std::string{listKey} + "</key>\n\t\t\t<array>\n";
    for (const std::string& item : list) {
        out += "\t\t\t\t<string>" + escaped(item) + "</string>\n";
    }
    out += "\t\t\t</array>\n\t\t</dict>\n";
}

int usage() {
    std::fputs("usage: xpcog-doctypes [output-file]\n"
               "Writes the Info.plist document-type entries for this build's codecs.\n",
               stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        return usage();
    }

    xpcog::PluginRegistry registry;
    xpcog::registerAllCodecs(registry);

    // Every extension this build opens, and as what. Decoders first, so that an
    // extension a container also claims stays audio; what is left is the
    // playlists and the archives, told apart by the container's name, which is
    // the one thing a descriptor says about itself.
    std::map<std::string, Kind> extensions;
    for (const std::string& extension : registry.allExtensions()) {
        extensions.emplace(extension, Kind::Audio);
    }
    for (const xpcog::ContainerDescriptor& container : registry.containers()) {
        const Kind kind = container.name.find("Archive") != std::string_view::npos
                              ? Kind::Archive
                              : Kind::Playlist;
        for (const std::string_view extension : container.extensions) {
            extensions.emplace(std::string{extension}, kind);
        }
    }

    std::vector<std::string> audioTypes;
    std::vector<std::string> playlistTypes;
    std::vector<std::string> archiveTypes;
    std::vector<std::string> byExtension;
    std::vector<Imported>    imported;
    std::vector<LeftOut>     leftOut;
    std::size_t              systemCount = 0;

    for (const auto& [extension, kind] : extensions) {
        std::vector<std::string>& types = kind == Kind::Audio      ? audioTypes
                                          : kind == Kind::Playlist ? playlistTypes
                                                                   : archiveTypes;
        if (const std::optional<SystemType> system = systemType(extension)) {
            if (!system->audiovisual) {
                leftOut.push_back({extension, *system});
                continue;
            }
            ++systemCount;
            appendUnique(types, system->identifier);
        } else {
            Imported entry{importedIdentifier(extension), extension, kind};
            appendUnique(types, entry.identifier);
            imported.push_back(std::move(entry));
        }
        byExtension.push_back(extension);
    }

    std::string out;
    out += "\t<key>CFBundleDocumentTypes</key>\n\t<array>\n";
    writeDocumentType(out, "Audio file", "Viewer", "Alternate", "LSItemContentTypes", audioTypes);
    writeDocumentType(out, "Playlist", "Viewer", "Alternate", "LSItemContentTypes", playlistTypes);
    writeDocumentType(out, "Archive of audio files", "Viewer", "Alternate", "LSItemContentTypes",
                      archiveTypes);
    writeDocumentType(out, "Audio file", "Viewer", "Alternate", "CFBundleTypeExtensions",
                      byExtension);
    writeDocumentType(out, "Folder", "None", "None", "LSItemContentTypes", {"public.folder"});
    out += "\t</array>\n";

    out += "\t<key>UTImportedTypeDeclarations</key>\n\t<array>\n";
    for (const Imported& entry : imported) {
        out += "\t\t<dict>\n";
        out += "\t\t\t<key>UTTypeIdentifier</key>\n\t\t\t<string>" + entry.identifier +
               "</string>\n";
        out += "\t\t\t<key>UTTypeDescription</key>\n\t\t\t<string>" +
               escaped(upper(entry.extension)) + noun(entry.kind) + "</string>\n";
        out += "\t\t\t<key>UTTypeConformsTo</key>\n\t\t\t<array>\n\t\t\t\t<string>";
        out += conformsTo(entry.kind);
        out += "</string>\n\t\t\t</array>\n";
        out += "\t\t\t<key>UTTypeTagSpecification</key>\n\t\t\t<dict>\n";
        out += "\t\t\t\t<key>public.filename-extension</key>\n\t\t\t\t<array>\n";
        out += "\t\t\t\t\t<string>" + escaped(entry.extension) + "</string>\n";
        out += "\t\t\t\t</array>\n\t\t\t</dict>\n\t\t</dict>\n";
    }
    out += "\t</array>\n";

    if (argc == 2) {
        std::ofstream file{argv[1], std::ios::binary | std::ios::trunc};
        if (!file || !(file << out)) {
            std::fprintf(stderr, "xpcog-doctypes: cannot write %s\n", argv[1]);
            return 1;
        }
    } else {
        std::fputs(out.c_str(), stdout);
    }

    std::fprintf(stderr,
                 "xpcog-doctypes: %zu extensions -- %zu with a type macOS declares, %zu "
                 "imported, %zu left out\n",
                 extensions.size(), systemCount, imported.size(), leftOut.size());
    for (const LeftOut& entry : leftOut) {
        std::fprintf(stderr, "  .%s is %s (%s)\n", entry.extension.c_str(),
                     entry.type.description.c_str(), entry.type.identifier.c_str());
    }
    return 0;
}
