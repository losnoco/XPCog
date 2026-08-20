#include "MiniFrame.hpp"

#include "AppIcon.hpp"
#include "Commands.hpp"
#include "LucideIcon.hpp"
#include "PlaybackController.hpp"
#include "SeekBar.hpp"
#include "Text.hpp"

#include <wx/bmpbuttn.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>

#include <cmath>

namespace xpcog::app {
namespace {

enum : int {
    kMiniSeekId = FirstWidgetId + 120,
    kMiniVolumeId,
};

[[nodiscard]] std::string formatClock(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const auto  total   = static_cast<int>(seconds + 0.5);
    const int   minutes = total / 60;
    const int   rest    = total % 60;
    const std::string padded =
        rest < 10 ? "0" + std::to_string(rest) : std::to_string(rest);
    return std::to_string(minutes) + ":" + padded;
}

}  // namespace

MiniFrame::MiniFrame(wxWindow* parent, PlaybackController& playback, Settings& settings)
    : wxFrame(parent, wxID_ANY, "XPCog", wxDefaultPosition, wxDefaultSize,
              wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU | wxCLIP_CHILDREN |
                  wxFRAME_TOOL_WINDOW),
      playback_(playback),
      settings_(settings) {
    SetIcons(applicationIcons());

    auto* panel = new wxPanel(this, wxID_ANY);
    auto* row   = new wxBoxSizer(wxHORIZONTAL);

    for (const CommandId id : transportLayout()) {
        auto* button = new wxBitmapButton(panel, id, lucideIcon(commandIcon(id)),
                                          wxDefaultPosition, wxDefaultSize,
                                          wxBORDER_NONE);
        button->SetBitmapDisabled(lucideIconDisabled(commandIcon(id)));
        row->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(2));
        buttons_.push_back(button);
    }

    seekBar_ = new SeekBar(panel, kMiniSeekId);
    row->Add(seekBar_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));

    clock_ = new wxStaticText(panel, wxID_ANY, "0:00", wxDefaultPosition,
                              FromDIP(wxSize(44, -1)), wxALIGN_CENTRE_HORIZONTAL);
    row->Add(clock_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

    volume_ = new wxSlider(panel, kMiniVolumeId,
                           static_cast<int>(playback_.volume() * 100.0), 0, 100,
                           wxDefaultPosition, FromDIP(wxSize(80, -1)));
    volume_->SetToolTip("Volume");
    row->Add(volume_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    panel->SetSizer(row);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(panel, 1, wxEXPAND);
    SetSizerAndFit(outer);
    SetMinSize(GetSize());

    // The transport buttons post the frame's own command ids, which the parent
    // handles -- so there is one Play/Pause implementation rather than two.
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
        wxCommandEvent forwarded(wxEVT_MENU, event.GetId());
        GetParent()->GetEventHandler()->ProcessEvent(forwarded);
    });

    subscriptions_.push_back(seekBar_->seekRequested.connect(
        [this](double seconds) { playback_.seek(seconds); }));
    subscriptions_.push_back(seekBar_->scrubbed.connect(
        [this](double seconds) { clock_->SetLabelText(toWx(formatClock(seconds))); }));

    volume_->Bind(wxEVT_SLIDER, [this](wxCommandEvent& event) {
        const double gain = event.GetInt() / 100.0;
        playback_.setVolume(gain);
        volumeChanged.publish(gain);
    });

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        // A mode, not a window: closing means "go back to the full window", so
        // the close is refused and the owner is told instead.
        event.Veto();
        dismissed.publish();
    });

    setFloating(settings_.FloatingMiniWindow());
}

void MiniFrame::setNowPlaying(const std::string& title, const std::string& artist) {
    if (title.empty()) {
        SetTitle("XPCog");
        return;
    }
    SetTitle(toWx(artist.empty() ? title : title + " \xE2\x80\x94 " + artist));
}

void MiniFrame::setPosition(double seconds, double duration) {
    duration_ = duration;
    seekBar_->setDuration(duration);
    seekBar_->setPosition(seconds);
    if (!seekBar_->scrubbing()) {
        clock_->SetLabelText(toWx(formatClock(seconds)));
    }
}

void MiniFrame::setPlaybackState(bool playing, bool paused) {
    (void)paused;
    if (!playing) {
        seekBar_->setDuration(0.0);
        clock_->SetLabelText("0:00");
    }
}

void MiniFrame::refreshVolume() {
    volume_->SetValue(static_cast<int>(std::lround(playback_.volume() * 100.0)));
}

void MiniFrame::setFloating(bool floating) {
    // In place. Qt needed the window destroyed and recreated to add or remove
    // Qt::WindowStaysOnTopHint, which lost the window's position every time the
    // setting was toggled; wx just changes the style.
    long style = GetWindowStyleFlag();
    if (floating) {
        style |= wxSTAY_ON_TOP;
    } else {
        style &= ~wxSTAY_ON_TOP;
    }
    SetWindowStyleFlag(style);
}

void MiniFrame::refreshIcons() {
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        const CommandId id = transportLayout()[i];
        buttons_[i]->SetBitmap(lucideIcon(commandIcon(id)));
        buttons_[i]->SetBitmapDisabled(lucideIconDisabled(commandIcon(id)));
    }
}

}  // namespace xpcog::app
