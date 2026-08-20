#include "AppIcon.hpp"

#include "icons_resources.hpp"

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/mstream.h>

#include <vector>

namespace xpcog::app {
namespace {

/// Kept in step with PNG_SIZES in app/icons/make-icons.py by the test that walks
/// this list against the embedded resource. Two lists in two languages is not
/// ideal, but the alternative is generating a header from the script, which makes
/// the build depend on ImageMagick to compile.
constexpr int kSizes[] = {16, 24, 32, 48, 64, 128, 256};

[[nodiscard]] wxBitmap decode(int size) {
    const std::span<const std::byte> bytes = resources::icons(applicationIconPath(size));
    if (bytes.empty()) {
        return wxNullBitmap;
    }
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxImage             image;
    if (!image.LoadFile(stream, wxBITMAP_TYPE_PNG)) {
        return wxNullBitmap;
    }
    return wxBitmap(image);
}

}  // namespace

wxBitmapBundle applicationIcon() {
    static const wxBitmapBundle icon = [] {
        wxVector<wxBitmap> bitmaps;
        for (const int size : kSizes) {
            if (wxBitmap bitmap = decode(size); bitmap.IsOk()) {
                bitmaps.push_back(bitmap);
            }
        }
        return wxBitmapBundle::FromBitmaps(bitmaps);
    }();
    return icon;
}

wxIconBundle applicationIcons() {
    static const wxIconBundle icons = [] {
        wxIconBundle built;
        for (const int size : kSizes) {
            if (wxIcon icon = applicationIconAt(size); icon.IsOk()) {
                built.AddIcon(icon);
            }
        }
        return built;
    }();
    return icons;
}

wxIcon applicationIconAt(int size) {
    wxIcon icon;
    if (wxBitmap bitmap = decode(size); bitmap.IsOk()) {
        icon.CopyFromBitmap(bitmap);
    }
    return icon;
}

std::string applicationIconPath(int size) {
    return "xpcog-" + std::to_string(size) + ".png";
}

std::vector<int> applicationIconSizes() {
    return std::vector<int>(std::begin(kSizes), std::end(kSizes));
}

}  // namespace xpcog::app
