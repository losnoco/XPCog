// MPRIS, the Linux desktop's media player protocol. The Linux counterpart of
// MacMediaIntegration.mm and WindowsMediaIntegration.cpp.
//
// MPRIS is not an API, it is a D-Bus contract: claim the well-known service name
// org.mpris.MediaPlayer2.<something>, export an object at
// /org/mpris/MediaPlayer2 implementing two interfaces, and every panel applet,
// lock screen, notification daemon and `playerctl` on the machine can drive
// playback. That is also why it is the richest of the three integrations -- it is
// a general remote control, where SMTC and MPNowPlayingInfoCenter are a card with
// buttons on it.
//
// Cog has no counterpart to port. This is the one media integration written from
// a specification rather than translated from Objective-C, which is worth saying
// because the usual check -- "does it do what Cog does" -- is unavailable. The
// reference is the MPRIS 2.2 spec; the observable behaviour is whatever
// `playerctl` and a desktop panel make of it.
//
// --- Why GDBus ------------------------------------------------------------
//
// This was QtDBus, whose QDBusAbstractAdaptor turned a Q_OBJECT into a D-Bus
// interface by introspecting its metaobject. Nothing else offers that, so the
// introspection XML, the property dispatch and the PropertiesChanged emission
// below are all hand-written where they used to be generated.
//
// GDBus rather than libdbus-1, for one decisive reason that has nothing to do
// with line count: a GTK application's main loop *is* a GMainLoop on the default
// main context, so an object registered from the GUI thread has its method calls
// delivered on the GUI thread. That is exactly the guarantee QtDBus was quietly
// providing. libdbus-1 would mean owning main-loop integration -- a pump thread
// and a marshalling hop back -- to rebuild something already true here.
//
// The dispatcher the other two platforms need is therefore a formality on this
// one. It is still used, because "an implementation never publishes directly" is
// a rule worth having exactly one of.
//
// GLib is not a new dependency: the toolkit's own package depends on GTK.
//
// --- Four things the spec makes non-obvious, all silent when wrong ---------
//
//   * Properties do not announce themselves. Registering an interface exports
//     its properties, but nothing emits
//     org.freedesktop.DBus.Properties.PropertiesChanged on its own -- so a panel
//     reads the metadata once and then shows the first track for the rest of the
//     session. Every setter here signals explicitly.
//   * Every time is in microseconds, and signed. Seconds would be wrong by a
//     factor of a million, in the direction that looks like a track of no length.
//   * mpris:trackid is an object path, not a string. A string goes into the
//     variant map without complaint and is then discarded by the consumer, which
//     appears as metadata that never shows up rather than as an error.
//   * Position must NOT be announced through PropertiesChanged. It changes
//     continuously, so announcing it would be a bus signal per tick to every
//     client on the machine. Clients extrapolate from PlaybackStatus and Rate,
//     and Seeked() is how they are told their extrapolation just became wrong.
//
// Position and seeking: MPRIS Seek() is *relative* and SetPosition() is absolute,
// while the engine and the other two platforms deal only in absolute positions.
// The relative case is resolved here, against the position last pushed in
// setPlaybackState() -- which is why that value is kept rather than just
// forwarded on.
//
// This file is compiled only on Linux, so it is not built on the host the rest of
// the port is being written on; CI's Linux job is what compiles it. What CI
// cannot do is tell whether a panel likes the result, so see docs/PORTING.md.

#include "xpcog/platform/MediaIntegration.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace xpcog::platform {
namespace {

constexpr const char* kObjectPath      = "/org/mpris/MediaPlayer2";
constexpr const char* kRootInterface   = "org.mpris.MediaPlayer2";
constexpr const char* kPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr const char* kPropertiesIface = "org.freedesktop.DBus.Properties";

/// Seconds to the microseconds every MPRIS time is expressed in.
constexpr double kMicrosPerSecond = 1'000'000.0;

/// Both interfaces, in the form g_dbus_node_info_new_for_xml() parses.
///
/// This is what QDBusAbstractAdaptor generated from Q_PROPERTY and Q_SLOTS. The
/// annotations matter: a property with no `access` defaults to readwrite and
/// makes a client offer to set something that will be refused, and `emits-changed`
/// tells a client whether to expect a signal or to poll. Position is marked
/// `false` deliberately -- see the note above about why it is never announced.
constexpr const char* kIntrospectionXml = R"XML(
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Play"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Seek">
      <arg name="Offset" type="x" direction="in"/>
    </method>
    <method name="SetPosition">
      <arg name="TrackId" type="o" direction="in"/>
      <arg name="Position" type="x" direction="in"/>
    </method>
    <method name="OpenUri">
      <arg name="Uri" type="s" direction="in"/>
    </method>
    <signal name="Seeked">
      <arg name="Position" type="x"/>
    </signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="Position" type="x" access="read">
      <annotation name="org.freedesktop.DBus.Property.EmitsChangedSignal" value="false"/>
    </property>
    <property name="Rate" type="d" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>
)XML";

/// `$XDG_CACHE_HOME/LoSnoCo/XPCog`, created. Where QStandardPaths::CacheLocation
/// pointed, organisation segment included.
[[nodiscard]] std::filesystem::path cacheDirectory() {
    const char* xdg  = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");

    std::filesystem::path base;
    if (xdg != nullptr && *xdg != '\0') {
        base = xdg;
    } else if (home != nullptr && *home != '\0') {
        base = std::filesystem::path{home} / ".cache";
    } else {
        return {};
    }

    const std::filesystem::path directory = base / "LoSnoCo" / "XPCog";
    std::error_code             ec;
    std::filesystem::create_directories(directory, ec);
    return ec ? std::filesystem::path{} : directory;
}

/// The file extension the bytes say they are, or empty when they say nothing
/// recognisable.
///
/// Sniffed rather than assumed because the artwork is now whatever the music file
/// carried -- usually JPEG, sometimes PNG -- where it used to be re-encoded to PNG
/// on the way here and the name could be a constant.
[[nodiscard]] const char* artworkExtension(const std::vector<std::byte>& bytes) {
    const auto byteAt = [&bytes](std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };
    if (bytes.size() >= 3 && byteAt(0) == 0xFF && byteAt(1) == 0xD8 && byteAt(2) == 0xFF) {
        return ".jpg";
    }
    if (bytes.size() >= 8 && byteAt(0) == 0x89 && byteAt(1) == 0x50 && byteAt(2) == 0x4E &&
        byteAt(3) == 0x47) {
        return ".png";
    }
    return "";
}

class GDbusMediaIntegration final : public MediaIntegration {
public:
    explicit GDbusMediaIntegration(Dispatcher dispatch)
        : MediaIntegration(std::move(dispatch)) {
        GError* error = nullptr;
        nodeInfo_     = g_dbus_node_info_new_for_xml(kIntrospectionXml, &error);
        if (nodeInfo_ == nullptr) {
            // A parse failure here is a bug in the literal above, not a runtime
            // condition, so it is worth saying out loud rather than degrading
            // silently the way a missing bus does.
            g_warning("MPRIS introspection XML did not parse: %s",
                      error != nullptr ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }

        // Suffixed with the process id, which the spec allows. Two players are
        // legal, and a stale unsuffixed name left by a crashed process would
        // otherwise make the *new* process the one without MPRIS.
        //
        // g_bus_own_name connects to the session bus itself and does nothing
        // observable when there is not one -- a headless session, or a container
        // -- which is the degradation the QtDBus version got from checking
        // isConnected() up front.
        std::string service = "org.mpris.MediaPlayer2.xpcog.instance" +
                              std::to_string(static_cast<long long>(getpid()));
        ownerId_ = g_bus_own_name(G_BUS_TYPE_SESSION, service.c_str(),
                                  G_BUS_NAME_OWNER_FLAGS_NONE, onBusAcquired,
                                  onNameAcquired, onNameLost, this, nullptr);
    }

    ~GDbusMediaIntegration() override {
        // Names first: unregistering an object while its name is still owned
        // leaves a window in which a client can call a method on an object that
        // has gone.
        if (ownerId_ != 0) {
            g_bus_unown_name(ownerId_);
        }
        if (connection_ != nullptr) {
            if (rootRegistration_ != 0) {
                g_dbus_connection_unregister_object(connection_, rootRegistration_);
            }
            if (playerRegistration_ != 0) {
                g_dbus_connection_unregister_object(connection_, playerRegistration_);
            }
            g_object_unref(connection_);
        }
        if (nodeInfo_ != nullptr) {
            g_dbus_node_info_unref(nodeInfo_);
        }
    }

    void setNowPlaying(const NowPlayingInfo& info) override {
        info_ = info;
        // A fresh id per track, so a SetPosition aimed at the previous one can be
        // told apart from one aimed at this.
        trackSerial_ += 1;
        artUrl_ = writeArtwork(info.artwork);
        notify("Metadata", buildMetadata());
    }

    void setPlaybackState(bool playing, bool paused, double position) override {
        const bool statusChanged = playing != playing_ || paused != paused_;
        playing_  = playing;
        paused_   = paused;
        position_ = position;

        // Only the status. See the file comment for why Position is not announced.
        if (statusChanged) {
            notify("PlaybackStatus", g_variant_new_string(playbackStatus()));
        }
    }

    void clear() override {
        info_ = {};
        artUrl_.clear();
        playing_  = false;
        paused_   = false;
        position_ = 0.0;
        notify("PlaybackStatus", g_variant_new_string(playbackStatus()));
        notify("Metadata", buildMetadata());
    }

    void setVolume(float gain) override {
        volume_ = gain;
        notify("Volume", g_variant_new_double(static_cast<double>(gain)));
    }

private:
    // --- registration ------------------------------------------------------

    static void onBusAcquired(GDBusConnection* connection, const gchar* /*name*/,
                              gpointer user_data) {
        auto* self = static_cast<GDbusMediaIntegration*>(user_data);
        self->registerObjects(connection);
    }

    static void onNameAcquired(GDBusConnection*, const gchar*, gpointer user_data) {
        static_cast<GDbusMediaIntegration*>(user_data)->registered_ = true;
    }

    static void onNameLost(GDBusConnection*, const gchar*, gpointer user_data) {
        // Also the "there is no session bus" path: GDBus reports a connection it
        // could not make as the name being lost.
        static_cast<GDbusMediaIntegration*>(user_data)->registered_ = false;
    }

    void registerObjects(GDBusConnection* connection) {
        connection_ = static_cast<GDBusConnection*>(g_object_ref(connection));

        static const GDBusInterfaceVTable kRootVTable{rootMethodCall, rootGetProperty,
                                                      nullptr, {}};
        static const GDBusInterfaceVTable kPlayerVTable{playerMethodCall,
                                                        playerGetProperty,
                                                        playerSetProperty, {}};

        // Two interfaces on one object path, registered separately -- which is
        // what QDBusConnection::ExportAdaptors did with the two adaptors.
        GError* error = nullptr;
        rootRegistration_ = g_dbus_connection_register_object(
            connection_, kObjectPath,
            g_dbus_node_info_lookup_interface(nodeInfo_, kRootInterface), &kRootVTable,
            this, nullptr, &error);
        if (rootRegistration_ == 0) {
            g_warning("MPRIS root interface not exported: %s",
                      error != nullptr ? error->message : "unknown");
            g_clear_error(&error);
        }

        playerRegistration_ = g_dbus_connection_register_object(
            connection_, kObjectPath,
            g_dbus_node_info_lookup_interface(nodeInfo_, kPlayerInterface),
            &kPlayerVTable, this, nullptr, &error);
        if (playerRegistration_ == 0) {
            g_warning("MPRIS player interface not exported: %s",
                      error != nullptr ? error->message : "unknown");
            g_clear_error(&error);
        }
    }

    // --- org.mpris.MediaPlayer2 -------------------------------------------

    static void rootMethodCall(GDBusConnection*, const gchar*, const gchar*,
                               const gchar*, const gchar* method,
                               GVariant* /*parameters*/,
                               GDBusMethodInvocation* invocation, gpointer user_data) {
        auto* self = static_cast<GDbusMediaIntegration*>(user_data);

        if (g_strcmp0(method, "Raise") == 0) {
            self->publishOnUiThread(self->raiseRequested);
        } else if (g_strcmp0(method, "Quit") == 0) {
            self->publishOnUiThread(self->quitRequested);
        }
        // Answered even for a method that is not ours: a caller left without a
        // reply blocks until its timeout.
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    static GVariant* rootGetProperty(GDBusConnection*, const gchar*, const gchar*,
                                     const gchar*, const gchar* property, GError**,
                                     gpointer /*user_data*/) {
        if (g_strcmp0(property, "CanQuit") == 0 || g_strcmp0(property, "CanRaise") == 0) {
            return g_variant_new_boolean(TRUE);
        }
        // No org.mpris.MediaPlayer2.TrackList. The playlist is not exported, and
        // saying otherwise makes a client call methods that are not there.
        if (g_strcmp0(property, "HasTrackList") == 0) {
            return g_variant_new_boolean(FALSE);
        }
        if (g_strcmp0(property, "Identity") == 0) {
            return g_variant_new_string("XPCog");
        }
        // The basename of an installed .desktop file, which is how a panel finds
        // the application's icon and localised name. Empty because XPCog installs
        // none yet: naming a file that does not exist gets no icon either way, and
        // adds a failed lookup.
        if (g_strcmp0(property, "DesktopEntry") == 0) {
            return g_variant_new_string("");
        }
        if (g_strcmp0(property, "SupportedUriSchemes") == 0) {
            const gchar* const schemes[] = {"file", nullptr};
            return g_variant_new_strv(schemes, -1);
        }
        // Deliberately coarse. An exact list would have to be derived from the
        // codec registry and kept in step with it, and MPRIS consults this only to
        // decide whether to offer OpenUri.
        if (g_strcmp0(property, "SupportedMimeTypes") == 0) {
            const gchar* const types[] = {"audio/*", nullptr};
            return g_variant_new_strv(types, -1);
        }
        return nullptr;
    }

    // --- org.mpris.MediaPlayer2.Player ------------------------------------

    static void playerMethodCall(GDBusConnection*, const gchar*, const gchar*,
                                 const gchar*, const gchar* method,
                                 GVariant* parameters,
                                 GDBusMethodInvocation* invocation,
                                 gpointer user_data) {
        auto* self = static_cast<GDbusMediaIntegration*>(user_data);

        if (g_strcmp0(method, "Play") == 0) {
            self->publishOnUiThread(self->playRequested);
        } else if (g_strcmp0(method, "Pause") == 0) {
            self->publishOnUiThread(self->pauseRequested);
        } else if (g_strcmp0(method, "PlayPause") == 0) {
            self->publishOnUiThread(self->playPauseRequested);
        } else if (g_strcmp0(method, "Stop") == 0) {
            self->publishOnUiThread(self->stopRequested);
        } else if (g_strcmp0(method, "Next") == 0) {
            self->publishOnUiThread(self->nextRequested);
        } else if (g_strcmp0(method, "Previous") == 0) {
            self->publishOnUiThread(self->previousRequested);
        } else if (g_strcmp0(method, "Seek") == 0) {
            gint64 offsetMicros = 0;
            g_variant_get(parameters, "(x)", &offsetMicros);
            // Clamped at zero rather than allowed to go negative: MPRIS says a
            // seek past the start lands at the start, not that it fails.
            const double target =
                std::max(0.0, self->position_ +
                                  (static_cast<double>(offsetMicros) / kMicrosPerSecond));
            self->announceSeek(target);
        } else if (g_strcmp0(method, "SetPosition") == 0) {
            const gchar* trackId       = nullptr;
            gint64       positionMicros = 0;
            g_variant_get(parameters, "(&ox)", &trackId, &positionMicros);
            // Drop a request aimed at a track that is no longer playing. The spec
            // requires this, and the race is real: a panel's seek bar sends the
            // position the user released the mouse on, and by then the track may
            // have ended -- applying it would jump the *next* track to a position
            // in the previous one.
            if (trackId != nullptr && self->trackPath() == trackId) {
                const double target =
                    std::max(0.0, static_cast<double>(positionMicros) / kMicrosPerSecond);
                self->announceSeek(target);
            }
        } else if (g_strcmp0(method, "OpenUri") == 0) {
            const gchar* uri = nullptr;
            g_variant_get(parameters, "(&s)", &uri);
            self->openUri(uri != nullptr ? uri : "");
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    static GVariant* playerGetProperty(GDBusConnection*, const gchar*, const gchar*,
                                       const gchar*, const gchar* property, GError**,
                                       gpointer user_data) {
        auto* self = static_cast<GDbusMediaIntegration*>(user_data);

        if (g_strcmp0(property, "PlaybackStatus") == 0) {
            return g_variant_new_string(self->playbackStatus());
        }
        if (g_strcmp0(property, "Metadata") == 0) {
            return self->buildMetadata();
        }
        if (g_strcmp0(property, "Volume") == 0) {
            return g_variant_new_double(static_cast<double>(self->volume_));
        }
        if (g_strcmp0(property, "Position") == 0) {
            return g_variant_new_int64(
                static_cast<gint64>(self->position_ * kMicrosPerSecond));
        }
        // Fixed at 1. The time-stretching DSP stages Cog uses to vary this are
        // deliberately not ported, so a range would advertise a control that does
        // nothing. Minimum and maximum both 1 is how MPRIS says "not adjustable".
        if (g_strcmp0(property, "Rate") == 0 || g_strcmp0(property, "MinimumRate") == 0 ||
            g_strcmp0(property, "MaximumRate") == 0) {
            return g_variant_new_double(1.0);
        }
        if (g_strcmp0(property, "CanGoNext") == 0 ||
            g_strcmp0(property, "CanGoPrevious") == 0 ||
            g_strcmp0(property, "CanPlay") == 0 || g_strcmp0(property, "CanPause") == 0 ||
            g_strcmp0(property, "CanSeek") == 0 ||
            g_strcmp0(property, "CanControl") == 0) {
            return g_variant_new_boolean(TRUE);
        }
        return nullptr;
    }

    static gboolean playerSetProperty(GDBusConnection*, const gchar*, const gchar*,
                                      const gchar*, const gchar* property,
                                      GVariant* value, GError**, gpointer user_data) {
        auto* self = static_cast<GDbusMediaIntegration*>(user_data);
        if (g_strcmp0(property, "Volume") != 0) {
            return FALSE;
        }
        const double gain = std::clamp(g_variant_get_double(value), 0.0, 1.0);
        self->publishOnUiThread(self->volumeRequested, static_cast<float>(gain));
        return TRUE;
    }

    // --- state -------------------------------------------------------------

    [[nodiscard]] const char* playbackStatus() const {
        if (!playing_) {
            return "Stopped";
        }
        return paused_ ? "Paused" : "Playing";
    }

    [[nodiscard]] std::string trackPath() const {
        return "/co/losno/xpcog/track/" + std::to_string(trackSerial_);
    }

    /// The Metadata property, as a floating `a{sv}`.
    [[nodiscard]] GVariant* buildMetadata() const {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

        // Required even when nothing is playing: a client that finds no trackid
        // treats the whole map as invalid.
        const std::string path = trackPath();
        g_variant_builder_add(&builder, "{sv}", "mpris:trackid",
                              g_variant_new_object_path(path.c_str()));

        if (info_.duration > 0.0) {
            // Omitted rather than zero when unknown, which is how a stream reports
            // itself. A zero length makes a client draw a seek bar of no width, and
            // some refuse to seek at all.
            g_variant_builder_add(
                &builder, "{sv}", "mpris:length",
                g_variant_new_int64(static_cast<gint64>(info_.duration * kMicrosPerSecond)));
        }
        if (!info_.title.empty()) {
            g_variant_builder_add(&builder, "{sv}", "xesam:title",
                                  g_variant_new_string(info_.title.c_str()));
        }
        if (!info_.artist.empty()) {
            // A list, not a string: xesam:artist is defined as an array, and a
            // client reading it as one gets an empty artist out of a bare string.
            const gchar* const artists[] = {info_.artist.c_str(), nullptr};
            g_variant_builder_add(&builder, "{sv}", "xesam:artist",
                                  g_variant_new_strv(artists, -1));
        }
        if (!info_.album.empty()) {
            g_variant_builder_add(&builder, "{sv}", "xesam:album",
                                  g_variant_new_string(info_.album.c_str()));
        }
        if (!artUrl_.empty()) {
            g_variant_builder_add(&builder, "{sv}", "mpris:artUrl",
                                  g_variant_new_string(artUrl_.c_str()));
        }
        return g_variant_builder_end(&builder);
    }

    /// PropertiesChanged, for one property. `value` is consumed.
    void notify(const char* property, GVariant* value) {
        if (connection_ == nullptr || !registered_) {
            // Still has to be consumed, or a floating reference leaks on every
            // transport tick before the bus is up.
            g_variant_unref(g_variant_ref_sink(value));
            return;
        }

        GVariantBuilder changed;
        g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&changed, "{sv}", property, value);

        GVariantBuilder invalidated;
        g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));

        g_dbus_connection_emit_signal(
            connection_, nullptr, kObjectPath, kPropertiesIface, "PropertiesChanged",
            g_variant_new("(sa{sv}as)", kPlayerInterface, &changed, &invalidated),
            nullptr);
    }

    /// Emits Seeked, the one position signal MPRIS does want: it tells clients
    /// their extrapolation is now wrong and to re-read Position. Then asks the
    /// player to actually go there.
    void announceSeek(double seconds) {
        position_ = seconds;
        if (connection_ != nullptr && registered_) {
            g_dbus_connection_emit_signal(
                connection_, nullptr, kObjectPath, kPlayerInterface, "Seeked",
                g_variant_new("(x)", static_cast<gint64>(seconds * kMicrosPerSecond)),
                nullptr);
        }
        publishOnUiThread(seekRequested, seconds);
    }

    /// Add and play a URL. Implemented rather than omitted because
    /// SupportedUriSchemes claims `file`, and a scheme advertised without this
    /// method is a promise the player does not keep.
    void openUri(const std::string& uri) {
        // Validated here rather than passed straight through. This is the one
        // method that takes an arbitrary string from any process on the bus, and
        // the scheme list says `file` -- so anything else is refused instead of
        // being handed to the playlist to fail on later, where the cause would be
        // invisible.
        const std::optional<Url> url = Url::parse(uri);
        if (!url.has_value() || url->scheme() != "file") {
            return;
        }
        publishOnUiThread(openUrlRequested, *url);
    }

    /// Artwork has to be a URL, so it has to be a file.
    ///
    /// MPRIS carries mpris:artUrl, not image bytes -- unlike SMTC, which takes a
    /// stream, and macOS, which takes an NSImage. One path, rewritten per track,
    /// rather than a temporary file each: the latter accumulates for the life of
    /// the process, and a long listening session is thousands of tracks.
    ///
    /// Rewriting in place has one consequence worth stating: a client caching by
    /// URL sees the same URL with different contents. In practice clients re-read
    /// on the Metadata signal, and the alternative trades that for the leak.
    [[nodiscard]] static std::string writeArtwork(const std::vector<std::byte>& artwork) {
        if (artwork.empty()) {
            return {};
        }
        const char* extension = artworkExtension(artwork);
        if (*extension == '\0') {
            return {};
        }
        const std::filesystem::path directory = cacheDirectory();
        if (directory.empty()) {
            return {};
        }

        const std::filesystem::path path = directory / (std::string{"mpris-art"} + extension);
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                return {};
            }
            out.write(reinterpret_cast<const char*>(artwork.data()),
                      static_cast<std::streamsize>(artwork.size()));
            if (!out) {
                return {};
            }
        }
        return Url::fromLocalPath(path).toString();
    }

    GDBusNodeInfo*   nodeInfo_   = nullptr;
    GDBusConnection* connection_ = nullptr;
    guint            ownerId_            = 0;
    guint            rootRegistration_   = 0;
    guint            playerRegistration_ = 0;
    bool             registered_         = false;

    NowPlayingInfo info_;
    std::string    artUrl_;
    bool           playing_     = false;
    bool           paused_      = false;
    double         position_    = 0.0;
    float          volume_      = 1.0F;
    std::uint64_t  trackSerial_ = 0;
};

}  // namespace

std::unique_ptr<MediaIntegration> MediaIntegration::create(Dispatcher dispatch,
                                                           void* nativeWindow) {
    // Unused here: MPRIS is a bus name, not a window property, unlike SMTC.
    (void)nativeWindow;
    return std::make_unique<GDbusMediaIntegration>(std::move(dispatch));
}

}  // namespace xpcog::platform
