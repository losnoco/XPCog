#include "SpectrumPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/audio/AudioTap.hpp"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/translation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace xpcog::app {
namespace {

/// 60 Hz. Cog redraws on a display link; a timer is the portable equivalent and
/// the difference is not visible on a bar chart -- the cursor advances by the
/// interval that was actually delivered, so a late timer costs a frame rather than
/// putting the display out of step with the music.
constexpr int kFrameIntervalMs = 16;

/// Cog divides the space between bars by this (bar_gap_denominator = 3): a third
/// of each slot is gap. Reproduced rather than guessed, because it is what makes a
/// spectrum look like Cog's rather than like a solid block.
constexpr int kGapDenominator = 3;

/// The dB gridlines Cog labels, drawn behind the bars.
constexpr int kGridLinesDb[] = {-10, -20, -30, -40, -50, -60, -70};

/// The background. Its own rather than the system's: bars on a light panel in a
/// dark theme, or the reverse, is what inheriting it produces.
const wxColour kBackground{18, 18, 20};

/// The grid's white, out of 255, and the line between the two halves of a
/// split view, which is the same line drawn firmly enough to read as an axis.
constexpr unsigned char kGridAlpha  = 22;
constexpr unsigned char kSplitAlpha = 48;

/// Qt's QColor::darker()/lighter(), which wx has no equivalent for. The factor is
/// a percentage as Qt spells it -- 160 is "160% darker", i.e. scaled by 100/160.
[[nodiscard]] wxColour shade(const wxColour& colour, int factor) {
    const auto scale = [factor](unsigned char channel) {
        const int scaled = (static_cast<int>(channel) * 100) / factor;
        return static_cast<unsigned char>(std::clamp(scaled, 0, 255));
    };
    return {scale(colour.Red()), scale(colour.Green()), scale(colour.Blue())};
}

struct ChannelsName {
    SpectrumPanel::Channels channels;
    const char*             key;
};

constexpr std::array<ChannelsName, 6> kChannelsNames = {{
    {SpectrumPanel::Channels::Mono, "mono"},
    {SpectrumPanel::Channels::Left, "left"},
    {SpectrumPanel::Channels::Right, "right"},
    {SpectrumPanel::Channels::Mirrored, "mirrored"},
    {SpectrumPanel::Channels::Stacked, "stacked"},
    {SpectrumPanel::Channels::Overlaid, "overlaid"},
}};

}  // namespace

const char* SpectrumPanel::channelsKey(Channels channels) noexcept {
    for (const ChannelsName& name : kChannelsNames) {
        if (name.channels == channels) {
            return name.key;
        }
    }
    return "mono";
}

SpectrumPanel::Channels SpectrumPanel::channelsFromKey(std::string_view key) noexcept {
    for (const ChannelsName& name : kChannelsNames) {
        if (key == name.key) {
            return name.channels;
        }
    }
    return Channels::Mono;
}

SpectrumPanel::SpectrumPanel(wxWindow* parent, AudioTap& tap, Settings& settings)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      tap_(tap),
      settings_(settings),
      timer_(this) {
    for (std::vector<float>& window : windows_) {
        window.assign(SpectrumAnalyzer::kWindowFrames, 0.0F);
    }

    // Not optional. Without it wx erases the background before the paint handler
    // runs, and the bars flicker at sixty frames a second.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(120, 48)));
    SetSize(FromDIP(wxSize(400, 120)));

    Bind(wxEVT_PAINT, &SpectrumPanel::onPaint, this);
    Bind(wxEVT_SIZE, &SpectrumPanel::onSize, this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    Bind(wxEVT_CONTEXT_MENU, &SpectrumPanel::onContextMenu, this);

    // Deliberately no wxEVT_SHOW handler, and this is the one comment in the file
    // worth reading twice.
    //
    // There was one, starting and stopping the clock as the pane appeared. It
    // crashed the application on exit, every time: tearing the frame down hides
    // its children, which sends wxEVT_SHOW to a window whose C++ object is
    // already going away, and the handler then read through it. An access
    // violation inside a window procedure surfaces as
    // STATUS_FATAL_USER_CALLBACK_EXCEPTION -- a bare exit code with no message
    // attached, which is why it took a vectored exception handler and a symbol
    // lookup to find rather than a guess.
    //
    // The handler was redundant anyway. Every path that shows or hides this pane
    // -- the View command, the pane's own close button, a change of playback
    // state -- already calls setActive(), because each of them knows something
    // wxEVT_SHOW does not: whether anything is playing.

    applySettings(settings_);
}

SpectrumPanel::~SpectrumPanel() { timer_.Stop(); }

void SpectrumPanel::setSampleRate(double rate) {
    for (SpectrumAnalyzer& analyzer : analyzers_) {
        analyzer.prepare(rate);
    }
    // And the cursor, which counts in frames and therefore cannot convert a frame
    // interval into one without knowing the rate.
    cursor_.setSampleRate(rate);
}

void SpectrumPanel::applySettings(const Settings& settings) {
    // Invalid colour text keeps whatever was there, rather than falling back to a
    // default colour -- which is black, and a black bar on a near-black background
    // is indistinguishable from the spectrum having stopped working. wxColour::Set
    // accepts "#rgb", "#rrggbb" and the SVG colour names, so a hand-edited settings
    // file has a fair chance of meaning what it says.
    if (wxColour bar; bar.Set(toWx(settings.SpectrumBarColor()))) {
        barColor_ = bar;
    }
    if (wxColour peak; peak.Set(toWx(settings.SpectrumDotColor()))) {
        peakColor_ = peak;
    }
    showPeaks_ = settings.SpectrumShowPeaks();

    const Channels channels = channelsFromKey(settings.SpectrumChannels());
    if (channels != channels_) {
        channels_ = channels;
        // The analysers hold the last frame's levels and peaks, and they were
        // read from a lane the new mode may not show. Cleared rather than left
        // to decay: a peak from the mix hanging over a bar of the left side is
        // a marker for a level nothing drew.
        for (SpectrumAnalyzer& analyzer : analyzers_) {
            analyzer.reset();
        }
    }

    for (SpectrumAnalyzer& analyzer : analyzers_) {
        analyzer.setFloorDb(settings.SpectrumFloorDb());
        analyzer.setMode(settings.SpectrumFreqMode() ? SpectrumAnalyzer::Mode::Frequencies
                                                     : SpectrumAnalyzer::Mode::NoteBands);
    }
    updateFrequencyBandCount();
    Refresh();
}

void SpectrumPanel::updateFrequencyBandCount() {
    if (analyzers_[0].mode() != SpectrumAnalyzer::Mode::Frequencies) {
        return;
    }
    const auto bars = static_cast<std::size_t>(
        std::max(1, GetClientSize().GetWidth() / FromDIP(kFrequencyBarPitch)));
    for (SpectrumAnalyzer& analyzer : analyzers_) {
        analyzer.setFrequencyBandCount(bars);
    }
}

bool SpectrumPanel::stereo() const noexcept {
    return channels_ == Channels::Mirrored || channels_ == Channels::Stacked ||
           channels_ == Channels::Overlaid;
}

void SpectrumPanel::onContextMenu(wxContextMenuEvent& event) {
    // The two choices that get flipped while looking at the bars -- which
    // channels, and whether the peaks are drawn -- and a way to the rest. The
    // colours, the band mode and the floor are only reachable through a
    // dialog that gives no hint it is where this pane is configured, and a
    // right-click on the thing itself is the shortest sentence for that.
    //
    // Cog has no such menu -- its spectrum is configured from Appearance and
    // that is all -- so this is an addition rather than a ported behaviour.
    // Ticks read from settings, so the menu says what the pane says.
    wxMenu menu;
    menu.AppendRadioItem(kMenuMono, _("Mono"));
    menu.AppendRadioItem(kMenuLeft, _("Left"));
    menu.AppendRadioItem(kMenuRight, _("Right"));
    menu.AppendRadioItem(kMenuMirrored, _("Stereo, mirrored"));
    menu.AppendRadioItem(kMenuStacked, _("Stereo, stacked"));
    menu.AppendRadioItem(kMenuOverlaid, _("Stereo, overlaid"));
    menu.AppendSeparator();
    menu.AppendCheckItem(kMenuPeaks, _("Show peak markers"));
    menu.AppendSeparator();
    // "Preferences..." verbatim, the same string the Pitch & Tempo pane's
    // button uses. One item on a menu popped from the spectrum does not need
    // to repeat where it came from, and reusing the message means it is
    // already translated.
    menu.Append(kMenuPreferences, trUtf8("Preferences\xE2\x80\xA6"));

    const Channels channels = channelsFromKey(settings_.SpectrumChannels());
    menu.Check(kMenuMono, channels == Channels::Mono);
    menu.Check(kMenuLeft, channels == Channels::Left);
    menu.Check(kMenuRight, channels == Channels::Right);
    menu.Check(kMenuMirrored, channels == Channels::Mirrored);
    menu.Check(kMenuStacked, channels == Channels::Stacked);
    menu.Check(kMenuOverlaid, channels == Channels::Overlaid);
    menu.Check(kMenuPeaks, settings_.SpectrumShowPeaks());

    // Screen coordinates from the event, except when the menu was asked for
    // from the keyboard -- wxDefaultPosition then, and wx documents popping it
    // wherever the window likes.
    const wxPoint where = event.GetPosition();
    const wxPoint at    = (where == wxDefaultPosition) ? wxPoint(0, 0) : ScreenToClient(where);

    // Synchronously rather than through a menu event: the id comes straight
    // back, so there is no routing to get wrong for a menu that outlives
    // nothing.
    const int chosen = GetPopupMenuSelectionFromUser(menu, at);
    if (chosen != wxID_NONE) {
        applyMenuItem(chosen);
    }
}

void SpectrumPanel::applyMenuItem(int item) {
    const auto channels = [this](Channels chosen) {
        settings_.setSpectrumChannels(channelsKey(chosen));
        settingChanged.publish("spectrumChannels");
    };
    switch (item) {
        case kMenuMono:     channels(Channels::Mono); break;
        case kMenuLeft:     channels(Channels::Left); break;
        case kMenuRight:    channels(Channels::Right); break;
        case kMenuMirrored: channels(Channels::Mirrored); break;
        case kMenuStacked:  channels(Channels::Stacked); break;
        case kMenuOverlaid: channels(Channels::Overlaid); break;
        case kMenuPeaks:
            settings_.setSpectrumShowPeaks(!settings_.SpectrumShowPeaks());
            settingChanged.publish("spectrumShowPeaks");
            break;
        case kMenuPreferences:
            settingsRequested.publish();
            break;
        default:
            break;
    }
}

void SpectrumPanel::onSize(wxSizeEvent& event) {
    event.Skip();
    // Only matters in Frequencies mode, where the bar count follows the width.
    // NoteBands has a fixed series and ignores it.
    updateFrequencyBandCount();
}

void SpectrumPanel::setActive(bool active) {
    playing_ = active;
    if (!active) {
        for (SpectrumAnalyzer& analyzer : analyzers_) {
            analyzer.reset();
        }
        Refresh();
    }
    // Visible *and* playing, or the clock stops. Either condition alone is a reason
    // not to be running a 4096-point transform sixty times a second.
    if (active && IsShownOnScreen()) {
        // The cursor starts again from wherever the audio has got to. Every path
        // that reaches here -- a track starting, a pause ending, the pane being
        // opened -- has a gap behind it that the display did not draw, and carrying
        // a position across one would put the window somewhere the music is not.
        cursor_.reset();
        lastTick_ = std::chrono::steady_clock::now();
        timer_.Start(kFrameIntervalMs);
    } else {
        timer_.Stop();
    }
}

void SpectrumPanel::tick() {
    const auto   now     = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_            = now;

    // Which lane the first analyser reads: the mix, one side, or the left of a
    // stereo pair. The tap fills every lane of a mono source with the same
    // sample, so a side of one is the sound rather than silence.
    const bool    both = stereo();
    const TapLane first = (channels_ == Channels::Mono)    ? TapLane::Mono
                          : (channels_ == Channels::Right) ? TapLane::Right
                                                           : TapLane::Left;

    // Nothing has played yet, or the writer lapped the window between the cursor
    // choosing it and the copy. Either way the last frame stays on screen, which is
    // the right thing to show: there is no new audio to draw, and clearing to
    // silence for one frame would be a flicker.
    if (!cursor_.read(tap_, elapsed, windows_[0].data(), windows_[0].size(), first)) {
        return;
    }
    // The same window, the other side, so the two halves of a stereo frame show
    // one instant. A failure here is the writer lapping between the reads,
    // which one frame later will not happen again.
    if (both && !cursor_.readAgain(tap_, windows_[1].data(), windows_[1].size(),
                                   TapLane::Right)) {
        return;
    }

    analyzers_[0].analyze(windows_[0].data(), windows_[0].size());
    if (both) {
        analyzers_[1].analyze(windows_[1].data(), windows_[1].size());
    }
    Refresh(false);
}

void SpectrumPanel::onPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(kBackground));
    dc.Clear();

    if (analyzers_[0].bands().empty()) {
        return;
    }

    const std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) {
        return;
    }

    const wxSize size   = GetClientSize();
    const auto   width  = static_cast<double>(size.GetWidth());
    const auto   height = static_cast<double>(size.GetHeight());
    if (width <= 0.0 || height <= 0.0) {
        return;
    }

    const SpectrumAnalyzer& left  = analyzers_[0];
    const SpectrumAnalyzer& right = analyzers_[1];

    switch (channels_) {
        case Channels::Mono:
        case Channels::Left:
        case Channels::Right:
            paintGrid(*gc, 0.0, height, false);
            paintBars(*gc, left, 0.0, height, false, barColor_, peakColor_);
            break;
        case Channels::Mirrored: {
            // Left rises from the centre line, right falls from it: the two
            // sides of one bar meet at the axis, so a difference between them
            // is a bar that is taller one way than the other.
            const double half = std::floor(height / 2.0);
            paintGrid(*gc, 0.0, half, false);
            paintGrid(*gc, half, height - half, true);
            paintBars(*gc, left, 0.0, half, false, barColor_, peakColor_);
            paintBars(*gc, right, half, height - half, true, barColor_, peakColor_);
            gc->SetPen(wxPen(wxColour(255, 255, 255, kSplitAlpha)));
            gc->StrokeLine(0.0, half, width, half);
            break;
        }
        case Channels::Stacked: {
            const double half = std::floor(height / 2.0);
            paintGrid(*gc, 0.0, half, false);
            paintGrid(*gc, half, height - half, false);
            paintBars(*gc, left, 0.0, half, false, barColor_, peakColor_);
            paintBars(*gc, right, half, height - half, false, barColor_, peakColor_);
            gc->SetPen(wxPen(wxColour(255, 255, 255, kSplitAlpha)));
            gc->StrokeLine(0.0, half, width, half);
            break;
        }
        case Channels::Overlaid:
            // Right first and darker, so the left -- the one most material
            // leads with -- is drawn over it in the true colour, as the
            // oscilloscope does. Where the right is louder its bar shows above
            // the left's, in the darker shade.
            paintGrid(*gc, 0.0, height, false);
            paintBars(*gc, right, 0.0, height, false, shade(barColor_, 160),
                      shade(peakColor_, 160));
            paintBars(*gc, left, 0.0, height, false, barColor_, peakColor_);
            break;
    }
}

void SpectrumPanel::paintGrid(wxGraphicsContext& gc, double top, double height,
                              bool flipped) {
    const auto width = static_cast<double>(GetClientSize().GetWidth());

    // Positioned by the same normalisation the bars use, so a line labelled
    // -40 dB really is where a -40 dB bar reaches. It follows the *configured*
    // floor rather than a fixed -80, or the grid would quietly start lying the
    // moment anyone changed it.
    const double floorDb = analyzers_[0].floorDb();
    gc.SetPen(wxPen(wxColour(255, 255, 255, kGridAlpha)));
    for (const int decibels : kGridLinesDb) {
        if (static_cast<double>(decibels) <= floorDb) {
            continue;  // below the floor, so off the bottom of the display
        }
        const double level = (static_cast<double>(decibels) - floorDb) / -floorDb;
        const double y =
            std::round(flipped ? top + (level * height) : top + height - (level * height));
        gc.StrokeLine(0.0, y, width, y);
    }
}

void SpectrumPanel::paintBars(wxGraphicsContext& gc, const SpectrumAnalyzer& analyzer,
                              double top, double height, bool flipped, const wxColour& bar,
                              const wxColour& peak) {
    const std::vector<float>& bands = analyzer.bands();
    const std::vector<float>& peaks = analyzer.peaks();
    if (bands.empty() || height <= 0.0) {
        return;
    }

    const auto   width    = static_cast<double>(GetClientSize().GetWidth());
    const double slot     = width / static_cast<double>(bands.size());
    const double gap      = slot / kGapDenominator;
    const double barWidth = std::max(1.0, slot - gap);

    // The edge the bars stand on, and the one they reach towards.
    const double base = flipped ? top : top + height;
    const double tip  = flipped ? top + height : top;

    // Base-to-tip gradient over the whole band rather than per bar, so a tall
    // bar and a short one agree about what a given height means -- per-bar
    // gradients make every bar look equally loud at its own tip.
    //
    // Derived from the chosen bar colour rather than a fixed ramp: the colour is a
    // setting, so anything hard-coded here would ignore it. Darker at the base
    // and lighter at the tip keeps the shape readable without inventing hues
    // nobody picked.
    gc.SetBrush(gc.CreateLinearGradientBrush(0.0, base, 0.0, tip, shade(bar, 160),
                                             shade(bar, 80)));
    gc.SetPen(*wxTRANSPARENT_PEN);

    for (std::size_t band = 0; band < bands.size(); ++band) {
        const double level = static_cast<double>(bands[band]);
        if (level <= 0.0) {
            continue;
        }
        const double x      = static_cast<double>(band) * slot;
        const double length = level * height;
        gc.DrawRectangle(x, flipped ? base : base - length, barWidth, length);
    }

    // The peak markers last, over the bars. Cog draws these as a one-pixel line in
    // its own colour -- spectrumDotColor -- and so does this.
    if (!showPeaks_) {
        return;
    }
    gc.SetPen(wxPen(peak));
    for (std::size_t band = 0; band < peaks.size(); ++band) {
        const double held = static_cast<double>(peaks[band]);
        if (held <= 0.0) {
            continue;
        }
        const double x = static_cast<double>(band) * slot;
        const double y = std::round(flipped ? base + (held * height) : base - (held * height));
        gc.StrokeLine(x, y, x + barWidth, y);
    }
}

}  // namespace xpcog::app
