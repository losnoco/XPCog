#include "Credits.hpp"

#include "Translations.hpp"

#include <array>

namespace xpcog::app {
namespace {

constexpr std::array kPlayer = {
    Component{"SQLite", "public domain", XPCOG_TRANSLATE("library database")},
    Component{"miniaudio", "public domain / MIT-0", XPCOG_TRANSLATE("audio output")},
    Component{"zlib", "zlib licence", XPCOG_TRANSLATE("decompression, throughout")},
    Component{"libcurl", "curl licence", XPCOG_TRANSLATE("HTTP and internet radio")},
    Component{"OpenSSL", "Apache-2.0", XPCOG_TRANSLATE("HTTPS, under libcurl")},
    Component{"nlohmann/json", "MIT", XPCOG_TRANSLATE("reading the web services' replies")},
    Component{"cpp-httplib", "MIT", XPCOG_TRANSLATE("the remote control's HTTP server")},
    Component{"Swagger UI", "Apache-2.0", XPCOG_TRANSLATE("the remote control's documentation page")},
    Component{"libsoxr", "LGPL-2.1", XPCOG_TRANSLATE("sample-rate conversion")},
    Component{"Rubber Band", "GPL-2.0", XPCOG_TRANSLATE("pitch and tempo")},
    Component{"Signalsmith Stretch", "MIT", XPCOG_TRANSLATE("pitch and tempo")},
    Component{"FreeSurround", "GPL-2.0", XPCOG_TRANSLATE("upmixing stereo to surround")},
    Component{"dsd2pcm", "BSD", XPCOG_TRANSLATE("DSD to PCM conversion")},
    Component{"hdcd_decode2", "BSD-2-Clause", XPCOG_TRANSLATE("HDCD decoding")},
    Component{"LPC extrapolation", "ISC-style", XPCOG_TRANSLATE("gapless edges")},
    Component{"sentry-native", "MIT", XPCOG_TRANSLATE("opt-in crash reporting")},
};

constexpr std::array kWx = {
    Component{"wxWidgets", "wxWindows Licence", XPCOG_TRANSLATE("user interface")},
    Component{"NanoSVG", "zlib licence", XPCOG_TRANSLATE("drawing the interface icons")},
    Component{"Lucide", "ISC", XPCOG_TRANSLATE("the interface icons themselves")},
};

constexpr std::array kGtk = {
    Component{"GTK", "LGPL-2.1-or-later", XPCOG_TRANSLATE("user interface")},
    Component{"libadwaita", "LGPL-2.1-or-later", XPCOG_TRANSLATE("user interface")},
    Component{"GLib", "LGPL-2.1-or-later", XPCOG_TRANSLATE("D-Bus, media keys and notifications")},
    Component{"libsecret", "LGPL-2.1-or-later", XPCOG_TRANSLATE("the password store")},
};

constexpr std::array kCodecs = {
    Component{"FLAC", "BSD-3-Clause", XPCOG_TRANSLATE("FLAC")},
    Component{"libogg / libvorbis", "BSD-3-Clause", XPCOG_TRANSLATE("Ogg Vorbis")},
    Component{"Opus / opusfile", "BSD-3-Clause", XPCOG_TRANSLATE("Opus")},
    Component{"minimp3", "CC0-1.0", XPCOG_TRANSLATE("MP3")},
    Component{"WavPack", "BSD-3-Clause", XPCOG_TRANSLATE("WavPack")},
    Component{"libmpcdec", "BSD-3-Clause", XPCOG_TRANSLATE("Musepack")},
    Component{"FFmpeg", "LGPL-2.1", XPCOG_TRANSLATE("AAC, ALAC, WMA and more")},
    Component{"TagLib", "LGPL-2.1 / MPL-1.1", XPCOG_TRANSLATE("tag reading")},
    Component{"libopenmpt", "BSD-3-Clause", XPCOG_TRANSLATE("tracker modules")},
    Component{"Game Music Emu", "LGPL-2.1", XPCOG_TRANSLATE("console chiptunes")},
    Component{"libarchive", "BSD-2-Clause", XPCOG_TRANSLATE("archives, and the SC-55 ROMs")},
    Component{"vgmstream", "ISC", XPCOG_TRANSLATE("game streaming formats")},
    Component{"libsidplayfp", "GPL-2.0", XPCOG_TRANSLATE("Commodore 64 SID")},
    Component{"AdPlug", "LGPL-2.1", XPCOG_TRANSLATE("AdLib and OPL2 formats")},
    Component{"libbinio", "LGPL-2.1", XPCOG_TRANSLATE("AdPlug's file reading")},
    Component{"libvgm", "GPL-2.0", XPCOG_TRANSLATE("VGM, S98, DRO and GYM")},
    Component{"Hively replayer", "BSD-3-Clause", XPCOG_TRANSLATE("AHX and Hively modules")},
    Component{"libjaytrax", "GPL-3.0-only", XPCOG_TRANSLATE("Syntrax modules")},
    Component{"SpessaSynth Core", "Apache-2.0", XPCOG_TRANSLATE("SoundFont synthesis")},
    Component{"Nuked OPL3", "GPL-2.0", XPCOG_TRANSLATE("OPL3 synthesis")},
    Component{"Nuked SC-55", "MAME licence", XPCOG_TRANSLATE("Roland SC-55 emulation")},
    Component{"psflib", "GPL-2.0", XPCOG_TRANSLATE("the PSF container")},
    Component{"HighlyExperimental", "GPL-2.0", XPCOG_TRANSLATE("PSF and PSF2 (PlayStation)")},
    Component{"HighlyQuixotic", "GPL-2.0", XPCOG_TRANSLATE("QSF (Capcom QSound)")},
    Component{"HighlyTheoretical", "GPL-3.0", XPCOG_TRANSLATE("DSF and SSF (Sega)")},
    Component{"lazyusf2", "GPL-2.0", XPCOG_TRANSLATE("USF (Nintendo 64)")},
    Component{"mGBA", "MPL-2.0", XPCOG_TRANSLATE("GSF (Game Boy Advance)")},
    Component{"snes9x", "Snes9x licence", XPCOG_TRANSLATE("SNSF (Super Nintendo)")},
    Component{"melonDS", "GPL-3.0-only", XPCOG_TRANSLATE("2SF (Nintendo DS)")},
    Component{"SSEQPlayer", "GPL-2.0", XPCOG_TRANSLATE("NCSF (Nintendo DS)")},
};

// Credited to whoever made them. For five of these nobody has stated a licence
// the player could rely on -- docs/PORTING.md's "Known gaps" says so, and Cog
// ships the same data under the same silence -- so the column names the rights
// holder rather than inventing terms.
constexpr std::array kData = {
    Component{"GeneralUser GS", "S. Christian Collins, SFe Team",
              XPCOG_TRANSLATE("the bundled SoundFont bank, free to use in software")},
    Component{"Cog equaliser presets", "the Cog authors, GPL-2.0-or-later", XPCOG_TRANSLATE("the equaliser's preset list")},
    Component{"YRW801 sample ROM", "Yamaha", XPCOG_TRANSLATE("OPL4 wavetable, under libvgm")},
    Component{"Commodore 64 ROMs", "Commodore", XPCOG_TRANSLATE("KERNAL, BASIC and character ROMs for SID")},
    Component{"PlayStation BIOS", "Sony", XPCOG_TRANSLATE("under HighlyExperimental")},
    Component{"Organya wavetable", "Pixel", XPCOG_TRANSLATE("Organya and PixTone sounds")},
    Component{"AdPlug song database", "the AdPlug authors", XPCOG_TRANSLATE("AdPlug's per-song fixes")},
};

}  // namespace

std::span<const Component> playerComponents() { return kPlayer; }
std::span<const Component> wxComponents() { return kWx; }
std::span<const Component> gtkComponents() { return kGtk; }
std::span<const Component> codecComponents() { return kCodecs; }
std::span<const Component> dataComponents() { return kData; }

}  // namespace xpcog::app
