#include "LucideIcon.hpp"

#include "icons_resources.hpp"

#include <wx/settings.h>

#include <map>
#include <string>
#include <utility>

namespace xpcog::app {
namespace {

/// The size the SVG is rendered at when a caller asks for no particular one.
/// wxBitmapBundle re-renders from the outline for other sizes and scale factors,
/// so this is a default rather than a limit -- unlike the Qt version, which had
/// to rasterise a fixed list of five sizes up front.
constexpr int kDefaultSize = 24;

/// Lucide's placeholder for "the colour of the surrounding text".
constexpr std::string_view kCurrentColor = "currentColor";

/// How much of the stroke the disabled state keeps. Greying a stroked outline
/// towards the window colour -- which is what a toolkit's own disabled rendering
/// does -- reads as a smudge; fading keeps the shape and loses the emphasis,
/// which is what "disabled" is meant to say.
constexpr double kDisabledOpacity = 0.35;

[[nodiscard]] std::string hex(const wxColour& colour) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           text      = "#";
    for (const unsigned char channel : {colour.Red(), colour.Green(), colour.Blue()}) {
        text += kDigits[channel >> 4];
        text += kDigits[channel & 0x0F];
    }
    return text;
}

/// Replaces every `currentColor`, and optionally dims the stroke.
[[nodiscard]] std::string stroke(std::string_view source, const wxColour& colour,
                                 double opacity) {
    std::string out;
    out.reserve(source.size() + 64);

    const std::string replacement = hex(colour);
    std::size_t       at          = 0;
    for (;;) {
        const std::size_t found = source.find(kCurrentColor, at);
        if (found == std::string_view::npos) {
            out.append(source.substr(at));
            break;
        }
        out.append(source.substr(at, found - at));
        out.append(replacement);
        at = found + kCurrentColor.size();
    }

    if (opacity < 1.0) {
        // On the root element, where Lucide already puts stroke and stroke-width,
        // so the children inherit it. nanosvg parses stroke-opacity; `opacity` as
        // a group property also works but is one more layer of its behaviour to
        // depend on, and these icons are stroke-only.
        const std::size_t tag = out.find("<svg");
        if (tag != std::string::npos) {
            out.insert(tag + 4, R"( stroke-opacity="0.35")");
        }
    }
    return out;
}

/// Keyed on name, colour and opacity together, so a change of system appearance
/// produces a new set rather than serving the old palette's icons for the rest of
/// the session.
using Cache = std::map<std::string, wxBitmapBundle>;

Cache& cache() {
    static Cache instances;
    return instances;
}

[[nodiscard]] wxColour defaultColour() {
    // Button text rather than window text: almost every one of these sits on a
    // toolbar button or a menu item, and on the platforms that distinguish them
    // it is the one that matches the label beside it.
    return wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
}

}  // namespace

std::string lucideIconPath(std::string_view name) {
    return "lucide/" + std::string{name} + ".svg";
}

wxBitmapBundle lucideIcon(std::string_view name, const wxColour& colour, double opacity) {
    const std::string key =
        std::string{name} + "@" + hex(colour) + (opacity < 1.0 ? "/dim" : "");

    if (const auto found = cache().find(key); found != cache().end()) {
        return found->second;
    }

    const std::span<const std::byte> source = resources::icons(lucideIconPath(name));
    if (source.empty()) {
        return cache().emplace(key, wxBitmapBundle{}).first->second;
    }

    const std::string stroked =
        stroke(std::string_view{reinterpret_cast<const char*>(source.data()), source.size()},
               colour, opacity);

    wxBitmapBundle bundle =
        wxBitmapBundle::FromSVG(stroked.c_str(), wxSize(kDefaultSize, kDefaultSize));
    return cache().emplace(key, std::move(bundle)).first->second;
}

wxBitmapBundle lucideIcon(std::string_view name) {
    return lucideIcon(name, defaultColour(), 1.0);
}

wxBitmapBundle lucideIconDisabled(std::string_view name) {
    return lucideIcon(name, defaultColour(), kDisabledOpacity);
}

void forgetLucideIcons() { cache().clear(); }

std::vector<std::string> lucideIconNames() {
    return {
        // Transport.
        "play",
        "pause",
        "square",
        "skip-back",
        "skip-forward",
        // View menu.
        "audio-lines",
        "sliders-vertical",
        "info",
        "panel-left",
        // Elsewhere.
        "folder-open",
        "x",
    };
}

}  // namespace xpcog::app
