#include "TrayMenu.hpp"

#include "Translations.hpp"

namespace xpcog::app {
namespace {

constexpr std::size_t kMaxTooltipRun = 60;

}  // namespace

std::string elideForTray(const std::string& text) {
    if (text.size() <= kMaxTooltipRun) {
        return text;
    }
    // Cut on a byte boundary that is not mid-sequence: UTF-8 continuation bytes
    // are 10xxxxxx, so walking back off them lands on a character start. A
    // tooltip ending in half a codepoint renders as a replacement glyph.
    std::size_t cut = kMaxTooltipRun;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut) + "\xE2\x80\xA6";
}

std::string trayTooltipBody(const TrayState& state) {
    std::string body;
    if (!state.title.empty()) {
        body = elideForTray(state.title);
        if (!state.artist.empty()) {
            body += "\n" + elideForTray(state.artist);
        }
    }
    if (state.playing && state.paused) {
        if (!body.empty()) {
            body += "\n";
        }
        body += tr("(paused)");
    }
    return body;
}

std::vector<platform::TrayMenuItem> trayMenuModel(const TrayState& state, bool withWindowItems) {
    // Id 0 is "no command", which is what the two track rows and the separators
    // are. Nothing can activate them, so nothing needs to know what they mean.
    constexpr int kNoCommand = 0;
    const auto    separator  = [] { return platform::TrayMenuItem{kNoCommand, {}, false, true}; };

    std::vector<platform::TrayMenuItem> items;

    // The track, as two disabled rows at the top, exactly as Cog's dock menu
    // does. Absent rather than blank when there is nothing: an empty row reads
    // as a broken menu.
    if (!state.title.empty()) {
        items.push_back({kNoCommand, elideForTray(state.title), false, false});
        if (!state.artist.empty()) {
            items.push_back({kNoCommand, elideForTray(state.artist), false, false});
        }
        items.push_back(separator());
    }

    items.push_back({PlaybackPlayPause, state.playing && !state.paused ? tr("Pause") : tr("Play"),
                     true, false});
    items.push_back({PlaybackStop, tr("Stop"), true, false});
    items.push_back(separator());
    items.push_back({PlaybackPrevious, tr("Previous"), true, false});
    items.push_back({PlaybackNext, tr("Next"), true, false});

    if (withWindowItems) {
        // Only where there is a tray. AppKit appends Quit and the window list
        // to a Dock menu itself, and clicking the Dock icon already raises the
        // window -- adding these there would produce a menu with two Quits.
        items.push_back(separator());
        items.push_back({kTrayShowWindowId, tr("Show XPCog"), true, false});
        items.push_back({FileQuit, tr("Quit"), true, false});
    }

    return items;
}

}  // namespace xpcog::app
