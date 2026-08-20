// ITaskbarList3: the overlay icon and the progress bar on the taskbar button.
//
// The Windows half of Cog's DockIconController. Two deliberate differences from
// what Cog draws, both because the surfaces are not the same shape:
//
//   * Cog badges its Dock tile for *every* state including stopped, because its
//     badge replaces the whole 1024px icon and something has to be there. A
//     Windows overlay is a 16px corner stamp on an icon that is already visible,
//     and the convention is that it appears only when there is something to say.
//     So playing and paused get an overlay and stopped clears it, rather than
//     stamping a permanent stop square on the taskbar.
//   * The glyphs are drawn here rather than shipped as assets. At 16 pixels a
//     triangle and two bars are a handful of arithmetic, and generating them means
//     they follow the display's scale factor instead of being a fixed bitmap that
//     goes soft at 150%.
//
// The drawing used to be four QPainter calls. It is now about sixty lines of
// coverage sampling, which is the honest cost of this layer linking no drawing
// library -- and the reason it is worth paying is that the alternative is
// dragging a GUI toolkit into the one directory whose whole job is talking to the
// OS. Sixteen samples per pixel on a 32x32 image is 16,384 point tests, done at
// most twice per track.
//
// COM: the toolkit initialises OLE on the GUI thread before any of this runs, so
// the apartment already exists. If CoCreateInstance fails anyway -- a stripped
// Windows install, a session with no shell -- every method degrades to doing
// nothing rather than the application failing to start over a taskbar decoration.

#include "xpcog/platform/TaskbarIntegration.hpp"

#include "WinString.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace xpcog::platform {
namespace {

/// The overlay is drawn at this size regardless of DPI; Windows scales it down to
/// the 16px corner. Larger than the target so a high-DPI taskbar has pixels to use.
constexpr int kOverlayPixels = 32;

/// Subsamples per axis. Sixteen samples a pixel is enough that a curve's edge
/// reads as smooth at the size this is actually seen at.
constexpr int kSubsamples = 4;

struct Rgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

constexpr Rgb kDisc{24.0, 24.0, 26.0};
constexpr Rgb kGlyph{245.0, 245.0, 245.0};

/// Whether `(px, py)` is inside the triangle `a`-`b`-`c`.
///
/// The three edge cross-products have to agree in sign. Written with `>=` on all
/// three so a point exactly on an edge counts as inside; the alternative leaves a
/// one-subsample seam along the hypotenuse.
[[nodiscard]] bool inTriangle(double px, double py, double ax, double ay, double bx,
                              double by, double cx, double cy) {
    const auto edge = [](double x0, double y0, double x1, double y1, double x, double y) {
        return ((x1 - x0) * (y - y0)) - ((y1 - y0) * (x - x0));
    };
    const double d0 = edge(ax, ay, bx, by, px, py);
    const double d1 = edge(bx, by, cx, cy, px, py);
    const double d2 = edge(cx, cy, ax, ay, px, py);
    return (d0 >= 0.0 && d1 >= 0.0 && d2 >= 0.0) ||
           (d0 <= 0.0 && d1 <= 0.0 && d2 <= 0.0);
}

/// A filled play triangle or pause bars on a dark disc, as premultiplied BGRA.
///
/// The disc is what makes it legible: the overlay lands on the bottom-right of the
/// application icon, whose colours are not ours to predict, and a bare glyph
/// disappears against the wrong one.
///
/// Premultiplied because that is what a 32-bit icon bitmap is blended as. The
/// colours here are opaque, so only the antialiased edges carry partial alpha,
/// and multiplying by coverage is exactly what accumulating per-subsample does.
[[nodiscard]] std::vector<std::uint8_t> glyphBitmap(bool paused) {
    constexpr double size = kOverlayPixels;
    constexpr double centre = size / 2.0;

    // The disc: a filled ellipse inset by one pixel, stroked two pixels wide and
    // centred on that path -- so the paint reaches half a pixel beyond it.
    constexpr double inset = 1.0;
    constexpr double radius = (size - (2.0 * inset)) / 2.0;
    constexpr double halfStroke = 1.0;

    // Pause bars, and the play triangle nudged right by a hair: a triangle centred
    // on its bounding box reads as sitting left of centre, because its visual mass
    // is not its bounding box.
    constexpr double barWidth  = size * 0.14;
    constexpr double barHeight = size * 0.40;
    constexpr double barTop    = (size - barHeight) / 2.0;
    constexpr double gap       = size * 0.10;
    constexpr double leftBar   = centre - gap - barWidth;
    constexpr double rightBar  = centre + gap;

    constexpr double triSize = size * 0.38;
    constexpr double triLeft = ((size - triSize) / 2.0) + (size * 0.04);
    constexpr double triTop  = (size - triSize) / 2.0;

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kOverlayPixels) *
                                     kOverlayPixels * 4, 0);

    constexpr double step  = 1.0 / kSubsamples;
    constexpr double first = step / 2.0;
    constexpr double total = kSubsamples * kSubsamples;

    for (int y = 0; y < kOverlayPixels; ++y) {
        for (int x = 0; x < kOverlayPixels; ++x) {
            Rgb    sum{};
            double covered = 0.0;

            for (int sy = 0; sy < kSubsamples; ++sy) {
                for (int sx = 0; sx < kSubsamples; ++sx) {
                    const double px = x + first + (sx * step);
                    const double py = y + first + (sy * step);

                    const double dx = px - centre;
                    const double dy = py - centre;
                    const double distance = std::sqrt((dx * dx) + (dy * dy));

                    if (distance > radius + halfStroke) {
                        continue;
                    }

                    const bool onStroke = distance >= radius - halfStroke;
                    const bool onGlyph =
                        !onStroke &&
                        (paused ? ((px >= leftBar && px < leftBar + barWidth) ||
                                   (px >= rightBar && px < rightBar + barWidth)) &&
                                      py >= barTop && py < barTop + barHeight
                                : inTriangle(px, py, triLeft, triTop, triLeft + triSize,
                                             triTop + (triSize / 2.0), triLeft,
                                             triTop + triSize));

                    const Rgb& colour = (onStroke || onGlyph) ? kGlyph : kDisc;
                    sum.r += colour.r;
                    sum.g += colour.g;
                    sum.b += colour.b;
                    covered += 1.0;
                }
            }

            if (covered == 0.0) {
                continue;
            }

            const auto scale = [](double channelSum) {
                return static_cast<std::uint8_t>(
                    std::clamp(std::lround(channelSum / total), 0L, 255L));
            };

            // BGRA in memory, matching the masks the header below declares.
            const std::size_t offset =
                ((static_cast<std::size_t>(y) * kOverlayPixels) + x) * 4;
            pixels[offset + 0] = scale(sum.b);
            pixels[offset + 1] = scale(sum.g);
            pixels[offset + 2] = scale(sum.r);
            pixels[offset + 3] = static_cast<std::uint8_t>(
                std::clamp(std::lround((covered / total) * 255.0), 0L, 255L));
        }
    }
    return pixels;
}

/// Turns premultiplied BGRA into an HICON.
///
/// The mask bitmap is required even for a 32-bit colour icon whose alpha does the
/// real work: CreateIconIndirect refuses an ICONINFO without one. It is left blank
/// -- all zero, meaning "opaque everywhere" -- and the alpha channel decides what
/// is actually seen.
[[nodiscard]] HICON iconFromPixels(const std::vector<std::uint8_t>& pixels) {
    BITMAPV5HEADER header{};
    header.bV5Size        = sizeof(BITMAPV5HEADER);
    header.bV5Width       = kOverlayPixels;
    header.bV5Height      = -kOverlayPixels;  // negative: top-down, as above
    header.bV5Planes      = 1;
    header.bV5BitCount    = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask     = 0x00FF0000;
    header.bV5GreenMask   = 0x0000FF00;
    header.bV5BlueMask    = 0x000000FF;
    header.bV5AlphaMask   = 0xFF000000;

    HDC   screen = GetDC(nullptr);
    void* bits   = nullptr;
    HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                      DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (colour == nullptr || bits == nullptr) {
        if (colour != nullptr) {
            DeleteObject(colour);
        }
        return nullptr;
    }

    std::memcpy(bits, pixels.data(), pixels.size());

    HBITMAP mask = CreateBitmap(kOverlayPixels, kOverlayPixels, 1, 1, nullptr);

    ICONINFO info{};
    info.fIcon    = TRUE;
    info.hbmColor = colour;
    info.hbmMask  = mask;

    HICON icon = CreateIconIndirect(&info);
    DeleteObject(colour);
    DeleteObject(mask);
    return icon;
}

/// The resolution the progress bar is reported at. ITaskbarList3 takes a
/// completed/total pair rather than a fraction, and a coarse total makes the bar
/// visibly step.
constexpr ULONGLONG kProgressResolution = 1000;

class WindowsTaskbarIntegration final : public TaskbarIntegration {
public:
    explicit WindowsTaskbarIntegration(HWND window) : window_(window) {
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&taskbar_)))) {
            // HrInit before anything else, and its failure is why the pointer is
            // released here: a half-initialised ITaskbarList3 accepts calls and
            // does nothing with them.
            if (FAILED(taskbar_->HrInit())) {
                taskbar_->Release();
                taskbar_ = nullptr;
            }
        }
    }

    ~WindowsTaskbarIntegration() override {
        clearOverlay();
        if (taskbar_ != nullptr) {
            taskbar_->Release();
        }
    }

    void setPlaybackState(bool playing, bool paused) override {
        if (taskbar_ == nullptr) {
            return;
        }
        if (!playing) {
            clearOverlay();
            taskbar_->SetOverlayIcon(window_, nullptr, nullptr);
            return;
        }

        HICON icon = iconFromPixels(glyphBitmap(paused));
        if (icon == nullptr) {
            return;
        }
        // The description is what a screen reader announces for the overlay, so it
        // says the state rather than naming the picture.
        taskbar_->SetOverlayIcon(window_, icon, paused ? L"Paused" : L"Playing");
        // Replaced after the call, not before: SetOverlayIcon copies what it needs,
        // but destroying the current icon while it is the one on screen leaves a
        // window with no overlay for as long as it takes to draw the next.
        clearOverlay();
        overlay_ = icon;
    }

    void setProgress(double fraction) override {
        if (taskbar_ == nullptr) {
            return;
        }
        const auto clamped = std::clamp(fraction, 0.0, 1.0);
        taskbar_->SetProgressState(window_, TBPF_NORMAL);
        taskbar_->SetProgressValue(
            window_, static_cast<ULONGLONG>(clamped * kProgressResolution),
            kProgressResolution);
    }

    void clearProgress() override {
        if (taskbar_ != nullptr) {
            taskbar_->SetProgressState(window_, TBPF_NOPROGRESS);
        }
    }

private:
    void clearOverlay() {
        if (overlay_ != nullptr) {
            DestroyIcon(overlay_);
            overlay_ = nullptr;
        }
    }

    HWND           window_  = nullptr;
    ITaskbarList3* taskbar_ = nullptr;
    HICON          overlay_ = nullptr;
};

}  // namespace

std::unique_ptr<TaskbarIntegration> TaskbarIntegration::create(void* nativeWindow) {
    if (nativeWindow == nullptr) {
        return std::make_unique<TaskbarIntegration>();
    }
    return std::make_unique<WindowsTaskbarIntegration>(static_cast<HWND>(nativeWindow));
}

}  // namespace xpcog::platform
