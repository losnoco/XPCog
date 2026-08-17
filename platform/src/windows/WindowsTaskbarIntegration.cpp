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
//     triangle and two bars are a few QPainter calls, and generating them means
//     they follow the display's scale factor instead of being a fixed bitmap that
//     goes soft at 150%.
//
// COM: Qt's Windows platform plugin calls OleInitialize on the GUI thread, so the
// apartment already exists by the time anything here runs. If CoCreateInstance
// fails anyway -- a stripped Windows install, a session with no shell -- every
// method degrades to doing nothing rather than the application failing to start
// over a taskbar decoration.

#include "xpcog/platform/TaskbarIntegration.hpp"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

namespace xpcog::platform {
namespace {

/// The overlay is drawn at this size regardless of DPI; Windows scales it down to
/// the 16px corner. Larger than the target so a high-DPI taskbar has pixels to use.
constexpr int kOverlayPixels = 32;

/// Turns a QImage into an HICON.
///
/// The mask bitmap is required even for a 32-bit colour icon whose alpha does the
/// real work: CreateIconIndirect refuses an ICONINFO without one. It is left blank
/// -- all zero, meaning "opaque everywhere" -- and the alpha channel decides what
/// is actually seen.
HICON iconFromImage(const QImage& source) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    BITMAPV5HEADER header{};
    header.bV5Size        = sizeof(BITMAPV5HEADER);
    header.bV5Width       = image.width();
    header.bV5Height      = -image.height();  // negative: top-down, as QImage is
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
        return nullptr;
    }

    std::memcpy(bits, image.constBits(),
                static_cast<std::size_t>(image.sizeInBytes()));

    HBITMAP mask = CreateBitmap(image.width(), image.height(), 1, 1, nullptr);

    ICONINFO info{};
    info.fIcon    = TRUE;
    info.hbmColor = colour;
    info.hbmMask  = mask;

    HICON icon = CreateIconIndirect(&info);
    DeleteObject(colour);
    DeleteObject(mask);
    return icon;
}

/// A filled play triangle or pause bars on a dark disc.
///
/// The disc is what makes it legible: the overlay lands on the bottom-right of the
/// application icon, whose colours are not ours to predict, and a bare glyph
/// disappears against the wrong one.
QImage glyphImage(bool paused) {
    QImage image(kOverlayPixels, kOverlayPixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    constexpr qreal inset = 1.0;
    const QRectF    disc(inset, inset, kOverlayPixels - (2 * inset),
                      kOverlayPixels - (2 * inset));
    painter.setBrush(QColor(24, 24, 26));
    painter.setPen(QPen(QColor(245, 245, 245), 2.0));
    painter.drawEllipse(disc);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(245, 245, 245));
    if (paused) {
        const qreal barWidth = kOverlayPixels * 0.14;
        const qreal barHeight = kOverlayPixels * 0.40;
        const qreal top      = (kOverlayPixels - barHeight) / 2.0;
        const qreal gap      = kOverlayPixels * 0.10;
        painter.drawRect(QRectF((kOverlayPixels / 2.0) - gap - barWidth, top,
                                barWidth, barHeight));
        painter.drawRect(QRectF((kOverlayPixels / 2.0) + gap, top, barWidth, barHeight));
    } else {
        // Nudged right by a hair: a triangle centred on its bounding box reads as
        // sitting left of centre, because its visual mass is not its bounding box.
        const qreal size = kOverlayPixels * 0.38;
        const qreal left = (kOverlayPixels - size) / 2.0 + (kOverlayPixels * 0.04);
        const qreal top  = (kOverlayPixels - size) / 2.0;
        QPainterPath triangle;
        triangle.moveTo(left, top);
        triangle.lineTo(left + size, top + (size / 2.0));
        triangle.lineTo(left, top + size);
        triangle.closeSubpath();
        painter.drawPath(triangle);
    }
    return image;
}

/// The resolution the progress bar is reported at. ITaskbarList3 takes a
/// completed/total pair rather than a fraction, and a coarse total makes the bar
/// visibly step.
constexpr ULONGLONG kProgressResolution = 1000;

class WindowsTaskbarIntegration final : public TaskbarIntegration {
public:
    WindowsTaskbarIntegration(WId window, QObject* parent)
        : TaskbarIntegration(parent), window_(reinterpret_cast<HWND>(window)) {
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

        HICON icon = iconFromImage(glyphImage(paused));
        if (icon == nullptr) {
            return;
        }
        // The description is what a screen reader announces for the overlay, so it
        // says the state rather than naming the picture.
        taskbar_->SetOverlayIcon(window_, icon,
                                 paused ? L"Paused" : L"Playing");
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

    HWND            window_  = nullptr;
    ITaskbarList3*  taskbar_ = nullptr;
    HICON           overlay_ = nullptr;
};

}  // namespace

TaskbarIntegration* TaskbarIntegration::create(WId window, QObject* parent) {
    return new WindowsTaskbarIntegration(window, parent);
}

}  // namespace xpcog::platform
