// The Roland SC-55's front panel, in step with what is coming out of the
// speakers. The counterpart of app/src/Sc55Panel.hpp; PanelFeed decides
// *when*, by holding states with a position in their track and answering for
// the position the speaker has reached, and this decides how the pixels get
// onto a widget.
//
// Where wx repacked every frame to 24-bit RGB because wxImage takes nothing
// else, GDK takes the emulator's buffer as it is: RGBX, a 1024-word stride,
// through gdk_memory_texture_new with the stride passed in. What it does not
// take is a *borrowed* buffer -- GTK uploads textures lazily, from a thread
// of its own choosing, so a texture over the live LCD buffer would be read
// while the next frame is composited into it. Each frame is copied into a
// GBytes of its own, which is a memcpy of the visible rows and no repack.
//
// The byte order is the one Sc55Panel.hpp explains: the emulator writes R,
// G, B into the low bytes of each word, which in memory on a little-endian
// machine is R, G, B, then the byte it calls alpha and that this ignores --
// GDK_MEMORY_R8G8B8X8.

#pragma once

#include "Glib.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace xpcog::gtk {

class Sc55View {
public:
    /// `position` reports where the speaker has reached in the current track,
    /// in seconds. A callback rather than a controller reference, because that
    /// is the whole of what this needs to know about playback.
    explicit Sc55View(std::function<double()> position);
    ~Sc55View();

    Sc55View(const Sc55View&)            = delete;
    Sc55View& operator=(const Sc55View&) = delete;

    [[nodiscard]] GtkWidget* widget() const { return stack_; }

    /// Starts and stops the refresh clock. Nothing is switched on in the
    /// *feed* here: states are recorded from the start of the track
    /// regardless, which is what lets this show the right one immediately.
    void setActive(bool active);

private:
    void tick();
    void showExplanation();

    std::function<double()> position_;
    Timeout                 timer_;
    std::vector<Connection> connections_;
    bool                    active_ = false;

    GObjectPtr<GtkWidget> owned_;
    GtkWidget* stack_   = nullptr;
    GtkWidget* picture_ = nullptr;
    GtkWidget* message_ = nullptr;

    /// The photograph the emulator composites onto, and the buffer it
    /// composites into. Both are the emulator's shapes, not ours.
    std::vector<std::uint32_t> background_;
    std::vector<std::uint32_t> buffer_;
};

}  // namespace xpcog::gtk
