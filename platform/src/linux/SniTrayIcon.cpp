// StatusNotifierItem, the tray protocol that outlived the tray.
//
// The Linux implementation of TrayIcon.hpp, which explains why this exists at all
// and what it deliberately leaves out. What follows is the protocol, and the
// parts of it that are silent when they are wrong.
//
// Two D-Bus interfaces, exported from this process, and both are needed:
//
//   org.kde.StatusNotifierItem   the icon, its tooltip and its state
//   com.canonical.dbusmenu       the menu, as a tree the panel walks
//
// They are separate specifications by separate projects that happen to be used
// together everywhere, which is why the second one is easy to miss. An SNI item
// whose Menu property points at nothing appears in the panel and does nothing
// when clicked -- so the failure looks like a bug in the icon rather than a menu
// that was never exported.
//
// Registration is not ownership. Owning the bus name is not enough: the host has
// to be *told*, by calling RegisterStatusNotifierItem on
// org.kde.StatusNotifierWatcher. Skipping that leaves an item that is correct,
// reachable, introspectable and invisible.
//
// --- Five things that are silent when wrong -------------------------------
//
//   * Pixmaps are ARGB32 in **network byte order**. Writing a uint32_t on a
//     little-endian machine gets BGRA, which renders as an icon with red and blue
//     swapped -- plausible enough on a coloured logo to survive a look.
//   * The menu is not announced by changing it. Nothing re-reads a layout on its
//     own; LayoutUpdated has to be emitted, carrying a revision that went up, or
//     the panel shows the first track's menu for the rest of the session. This is
//     the same trap PropertiesChanged is in GDbusMediaIntegration.cpp, and it
//     bites in the same place.
//   * dbusmenu item id 0 is the root. An item that uses it is a menu whose root
//     is one of its own children, and the panel draws nothing.
//   * `_` in a dbusmenu label is a mnemonic marker, the way `&` is in a toolkit.
//     Track titles contain underscores, and an un-escaped one silently eats the
//     character after it.
//   * A method call with no reply blocks the caller until its timeout. That
//     caller is the panel, so a method left unhandled is a panel that stops
//     drawing for twenty-five seconds. Every method here answers, including the
//     ones that do nothing.
//
// --- Menu ids -------------------------------------------------------------
//
// The menu is pushed whole and rebuilt on every track change, so the ids in it
// have to mean something across a rebuild: a listener who opens the menu just as
// a track ends and then clicks Pause must get Pause. So a row's dbusmenu id *is*
// the application's command id, which is a constant -- not its position, which
// moves the moment the two now-playing rows appear or vanish. The rows that carry
// no command (the separators, and the disabled track labels) are numbered from a
// range far above any command id; nothing can click them, so a stale one is
// harmless.
//
// Compiled only on Linux. CI's Linux job builds it; whether a panel likes the
// result is the part CI cannot answer.

#include "xpcog/platform/TrayIcon.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xpcog::platform {
namespace {

constexpr const char* kItemInterface = "org.kde.StatusNotifierItem";
constexpr const char* kItemPath      = "/StatusNotifierItem";
constexpr const char* kMenuInterface = "com.canonical.dbusmenu";
constexpr const char* kMenuPath      = "/MenuBar";
constexpr const char* kWatcherName   = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath   = "/StatusNotifierWatcher";

/// The one blocking call this makes is at construction, and a session bus that
/// does not answer must not hold the window up behind it.
constexpr int kBusTimeoutMs = 2000;

/// Where the ids of rows that carry no command start. Far above any wx command
/// id, which is what keeps the two ranges from meeting.
constexpr gint32 kFillerIdBase = 1'000'000;

/// The dbusmenu revision this speaks. 3 is what the specification settled on and
/// what every host implements.
constexpr guint32 kMenuVersion = 3;

constexpr const char* kIntrospectionXml = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus">
      <arg name="status" type="s"/>
    </signal>
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
  </interface>
  <interface name="com.canonical.dbusmenu">
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg name="events" type="a(isvu)" direction="in"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="updatesNeeded" type="ai" direction="out"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/>
      <arg name="parent" type="i"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg name="id" type="i"/>
      <arg name="timestamp" type="u"/>
    </signal>
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
  </interface>
</node>
)XML";

/// `_` marks the character after it as a mnemonic, so a literal one is doubled.
///
/// The labels that need this are the track title and artist, which are whatever
/// the file said. Without it, "Bad_Apple" loses its underscore and underlines the
/// A, which looks like a rendering quirk rather than an escaping bug.
[[nodiscard]] std::string escapeLabel(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        if (character == '_') {
            escaped += '_';
        }
        escaped += character;
    }
    return escaped;
}

class SniTrayIcon final : public TrayIcon {
public:
    explicit SniTrayIcon(Dispatcher dispatch) : TrayIcon(std::move(dispatch)) {
        GError* error = nullptr;
        nodeInfo_     = g_dbus_node_info_new_for_xml(kIntrospectionXml, &error);
        if (nodeInfo_ == nullptr) {
            // A bug in the literal above, not a runtime condition, so it is said
            // out loud rather than degrading quietly the way a missing bus does.
            g_warning("Tray introspection XML did not parse: %s",
                      error != nullptr ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }

        // Synchronous, and the only blocking call here. isAvailable() has to be
        // answerable the moment this returns, because the caller decides on the
        // strength of it whether closing the window may hide it -- and an answer
        // that arrives a round trip later arrives after that decision.
        connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (connection_ == nullptr) {
            // No session bus: a headless login, or a container. Not worth a
            // warning, and the base class's answer is already the right one.
            g_clear_error(&error);
            return;
        }

        available_ = watcherPresent();

        registerObjects();

        // The suffix is the convention hosts expect, and some parse the pid out
        // of it. The trailing 1 is the item number within the process; XPCog
        // publishes exactly one.
        busName_ = "org.kde.StatusNotifierItem-" +
                   std::to_string(static_cast<long long>(getpid())) + "-1";
        ownerId_ = g_bus_own_name_on_connection(connection_, busName_.c_str(),
                                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                                onNameAcquired, onNameLost, this,
                                                nullptr);

        // Watched rather than checked once, so that a shell restart -- or the
        // AppIndicator extension being switched off and on, which is how somebody
        // debugging this will find out it works -- gets the item back instead of
        // needing the player restarted.
        watcherId_ = g_bus_watch_name_on_connection(
            connection_, kWatcherName, G_BUS_NAME_WATCHER_FLAGS_NONE,
            onWatcherAppeared, onWatcherVanished, this, nullptr);
    }

    ~SniTrayIcon() override {
        remove();
        if (connection_ != nullptr) {
            g_object_unref(connection_);
            connection_ = nullptr;
        }
        if (nodeInfo_ != nullptr) {
            g_dbus_node_info_unref(nodeInfo_);
            nodeInfo_ = nullptr;
        }
    }

    [[nodiscard]] bool isAvailable() const override { return available_; }

    void setIcon(const std::vector<TrayImage>& sizes) override {
        icons_ = sizes;
        emitItemSignal("NewIcon", nullptr);
    }

    void setToolTip(const std::string& title, const std::string& body) override {
        toolTipTitle_ = title;
        toolTipBody_  = body;
        emitItemSignal("NewToolTip", nullptr);
    }

    void setMenu(const std::vector<TrayMenuItem>& items) override {
        items_ = items;

        // Every rebuild is a new revision, whether or not the rows moved. A host
        // compares revisions rather than contents, so a bump that did not need to
        // happen costs one re-read and a missed one costs a stale menu for the
        // rest of the session.
        revision_ += 1;
        emitMenuSignal("LayoutUpdated", g_variant_new("(ui)", revision_, 0));
    }

    void remove() override {
        // The name goes first. Unregistering the objects while the name is still
        // owned leaves a window in which a panel can call a method on an object
        // that is no longer there.
        if (watcherId_ != 0) {
            g_bus_unwatch_name(watcherId_);
            watcherId_ = 0;
        }
        if (ownerId_ != 0) {
            g_bus_unown_name(ownerId_);
            ownerId_ = 0;
        }
        nameOwned_ = false;

        if (connection_ != nullptr) {
            if (itemRegistration_ != 0) {
                g_dbus_connection_unregister_object(connection_, itemRegistration_);
                itemRegistration_ = 0;
            }
            if (menuRegistration_ != 0) {
                g_dbus_connection_unregister_object(connection_, menuRegistration_);
                menuRegistration_ = 0;
            }
        }
        available_ = false;
    }

private:
    // --- registration ------------------------------------------------------

    /// Whether a host is on the bus right now, asked directly rather than waited
    /// for. See the constructor for why this one call is synchronous.
    [[nodiscard]] bool watcherPresent() const {
        GVariant* reply = g_dbus_connection_call_sync(
            connection_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", kWatcherName),
            G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, kBusTimeoutMs, nullptr,
            nullptr);
        if (reply == nullptr) {
            return false;
        }
        gboolean present = FALSE;
        g_variant_get(reply, "(b)", &present);
        g_variant_unref(reply);
        return present != FALSE;
    }

    void registerObjects() {
        static const GDBusInterfaceVTable kItemVTable{itemMethodCall, itemGetProperty,
                                                      nullptr, {}};
        static const GDBusInterfaceVTable kMenuVTable{menuMethodCall, menuGetProperty,
                                                      nullptr, {}};

        GError* error = nullptr;
        itemRegistration_ = g_dbus_connection_register_object(
            connection_, kItemPath,
            g_dbus_node_info_lookup_interface(nodeInfo_, kItemInterface), &kItemVTable,
            this, nullptr, &error);
        if (itemRegistration_ == 0) {
            g_warning("Tray item interface not exported: %s",
                      error != nullptr ? error->message : "unknown");
            g_clear_error(&error);
        }

        menuRegistration_ = g_dbus_connection_register_object(
            connection_, kMenuPath,
            g_dbus_node_info_lookup_interface(nodeInfo_, kMenuInterface), &kMenuVTable,
            this, nullptr, &error);
        if (menuRegistration_ == 0) {
            g_warning("Tray menu interface not exported: %s",
                      error != nullptr ? error->message : "unknown");
            g_clear_error(&error);
        }
    }

    static void onNameAcquired(GDBusConnection*, const gchar*, gpointer user_data) {
        auto* self      = static_cast<SniTrayIcon*>(user_data);
        self->nameOwned_ = true;
        self->registerWithWatcher();
    }

    static void onNameLost(GDBusConnection*, const gchar*, gpointer user_data) {
        static_cast<SniTrayIcon*>(user_data)->nameOwned_ = false;
    }

    static void onWatcherAppeared(GDBusConnection*, const gchar*, const gchar*,
                                  gpointer user_data) {
        auto* self       = static_cast<SniTrayIcon*>(user_data);
        self->available_ = true;
        self->registerWithWatcher();
    }

    static void onWatcherVanished(GDBusConnection*, const gchar*, gpointer user_data) {
        // Answered honestly rather than left optimistic: the caller uses this to
        // decide whether hiding its window leaves something to click, and after
        // the panel has gone it does not.
        static_cast<SniTrayIcon*>(user_data)->available_ = false;
    }

    /// Tell the host we exist. The step that is easy to leave out, and leaving it
    /// out produces an item that is correct in every other respect and invisible.
    void registerWithWatcher() {
        if (connection_ == nullptr || !nameOwned_ || !available_) {
            return;
        }
        // Fire and forget, with no callback: a reply we would do nothing with is
        // a reply that could arrive after this object has gone.
        g_dbus_connection_call(connection_, kWatcherName, kWatcherPath, kWatcherName,
                               "RegisterStatusNotifierItem",
                               g_variant_new("(s)", busName_.c_str()), nullptr,
                               G_DBUS_CALL_FLAGS_NONE, kBusTimeoutMs, nullptr, nullptr,
                               nullptr);
    }

    // --- org.kde.StatusNotifierItem ---------------------------------------

    static void itemMethodCall(GDBusConnection*, const gchar*, const gchar*,
                               const gchar*, const gchar* method, GVariant*,
                               GDBusMethodInvocation* invocation, gpointer user_data) {
        auto* self = static_cast<SniTrayIcon*>(user_data);

        if (g_strcmp0(method, "Activate") == 0) {
            self->publishOnUiThread(self->activated);
        }
        // ContextMenu is not handled, and that is not an omission: a host that
        // reads the Menu property pops the menu up itself and never calls this.
        // The ones that do call it are asking us to place a menu on screen, which
        // this layer has no toolkit to do -- so it answers and does nothing,
        // rather than leaving the panel waiting on a reply that never comes.
        //
        // SecondaryActivate and Scroll are middle-click and the scroll wheel.
        // Deliberately unbound: guessing that a wheel over the icon means volume
        // is exactly the kind of behaviour that should be asked for.
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    static GVariant* itemGetProperty(GDBusConnection*, const gchar*, const gchar*,
                                     const gchar*, const gchar* property, GError**,
                                     gpointer user_data) {
        auto* self = static_cast<SniTrayIcon*>(user_data);

        // ApplicationStatus rather than something more descriptive: the categories
        // are a fixed list, panels sort and group by them, and a player is an
        // application with a status rather than a piece of hardware.
        if (g_strcmp0(property, "Category") == 0) {
            return g_variant_new_string("ApplicationStatus");
        }
        if (g_strcmp0(property, "Id") == 0) {
            return g_variant_new_string("xpcog");
        }
        if (g_strcmp0(property, "Title") == 0) {
            return g_variant_new_string("XPCog");
        }
        // Always shown. Passive lets a panel hide the item, which for the one
        // control that is meant to survive the window being hidden is the wrong
        // way round.
        if (g_strcmp0(property, "Status") == 0) {
            return g_variant_new_string("Active");
        }
        if (g_strcmp0(property, "WindowId") == 0) {
            return g_variant_new_int32(0);
        }
        if (g_strcmp0(property, "IconPixmap") == 0) {
            return self->buildPixmaps();
        }
        if (g_strcmp0(property, "ToolTip") == 0) {
            return self->buildToolTip();
        }
        // False, so that a left click arrives as Activate and raises the window,
        // and the menu stays on the right button. True would make the icon a menu
        // and nothing else, which loses the one gesture that has an obvious
        // meaning.
        if (g_strcmp0(property, "ItemIsMenu") == 0) {
            return g_variant_new_boolean(FALSE);
        }
        if (g_strcmp0(property, "Menu") == 0) {
            return g_variant_new_object_path(kMenuPath);
        }
        // The themed-icon and attention properties, all empty. They have to
        // answer -- a property that returns nothing is an error reply, and a host
        // reading the set in one go gets that instead of the rest -- but XPCog
        // ships its icon as pixmaps and has no attention state to signal.
        if (g_strcmp0(property, "IconName") == 0 ||
            g_strcmp0(property, "OverlayIconName") == 0 ||
            g_strcmp0(property, "AttentionIconName") == 0 ||
            g_strcmp0(property, "AttentionMovieName") == 0) {
            return g_variant_new_string("");
        }
        if (g_strcmp0(property, "OverlayIconPixmap") == 0 ||
            g_strcmp0(property, "AttentionIconPixmap") == 0) {
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
            return g_variant_builder_end(&builder);
        }
        return nullptr;
    }

    /// The icon, every size at once, as `a(iiay)`.
    [[nodiscard]] GVariant* buildPixmaps() const {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
        for (const TrayImage& image : icons_) {
            if (image.width <= 0 || image.height <= 0 || image.argb.empty()) {
                continue;
            }
            // g_variant_new_fixed_array copies, which is what makes handing it a
            // pointer into a member safe against the next setIcon().
            GVariant* bytes = g_variant_new_fixed_array(
                G_VARIANT_TYPE_BYTE, image.argb.data(), image.argb.size(), 1);
            g_variant_builder_add(&builder, "(ii@ay)", image.width, image.height, bytes);
        }
        return g_variant_builder_end(&builder);
    }

    /// `(sa(iiay)ss)` -- icon name, icon pixmaps, title, body.
    [[nodiscard]] GVariant* buildToolTip() const {
        GVariantBuilder pixmaps;
        g_variant_builder_init(&pixmaps, G_VARIANT_TYPE("a(iiay)"));
        // Empty on purpose. The panel already has the item's icon and draws it
        // beside the tooltip; repeating it here gets it drawn twice.
        return g_variant_new("(sa(iiay)ss)", "", &pixmaps, toolTipTitle_.c_str(),
                             toolTipBody_.c_str());
    }

    // --- com.canonical.dbusmenu -------------------------------------------

    static void menuMethodCall(GDBusConnection*, const gchar*, const gchar*,
                               const gchar*, const gchar* method, GVariant* parameters,
                               GDBusMethodInvocation* invocation, gpointer user_data) {
        auto* self = static_cast<SniTrayIcon*>(user_data);

        if (g_strcmp0(method, "GetLayout") == 0) {
            gint32 parentId = 0;
            g_variant_get_child(parameters, 0, "i", &parentId);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(u@(ia{sv}av))", self->revision_,
                                          self->buildLayout(parentId)));
            return;
        }
        if (g_strcmp0(method, "GetGroupProperties") == 0) {
            g_dbus_method_invocation_return_value(invocation,
                                                  self->buildGroupProperties(parameters));
            return;
        }
        if (g_strcmp0(method, "GetProperty") == 0) {
            gint32       id   = 0;
            const gchar* name = nullptr;
            g_variant_get(parameters, "(i&s)", &id, &name);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(v)", self->propertyOf(id, name)));
            return;
        }
        if (g_strcmp0(method, "Event") == 0) {
            gint32       id      = 0;
            const gchar* eventId = nullptr;
            g_variant_get_child(parameters, 0, "i", &id);
            g_variant_get_child(parameters, 1, "&s", &eventId);
            self->handleEvent(id, eventId != nullptr ? eventId : "");
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (g_strcmp0(method, "EventGroup") == 0) {
            self->handleEventGroup(parameters);
            // No id failed, which is what an empty array says.
            GVariantBuilder errors;
            g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(ai)", &errors));
            return;
        }
        if (g_strcmp0(method, "AboutToShow") == 0) {
            // True, always. It costs one GetLayout on a gesture the listener just
            // made, and it buys a menu that cannot be stale -- the play/pause row
            // and the two track rows change under this menu constantly, and a
            // panel that missed a LayoutUpdated would otherwise show the previous
            // track until something else moved.
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
            return;
        }
        if (g_strcmp0(method, "AboutToShowGroup") == 0) {
            GVariantBuilder updates;
            g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
            GVariantBuilder errors;
            g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(aiai)", &updates, &errors));
            return;
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    static GVariant* menuGetProperty(GDBusConnection*, const gchar*, const gchar*,
                                     const gchar*, const gchar* property, GError**,
                                     gpointer /*user_data*/) {
        if (g_strcmp0(property, "Version") == 0) {
            return g_variant_new_uint32(kMenuVersion);
        }
        // Not read from the locale, which this layer does not know. It is a hint
        // for laying out a menu the panel draws with its own widgets, and those
        // follow the desktop's direction anyway.
        if (g_strcmp0(property, "TextDirection") == 0) {
            return g_variant_new_string("ltr");
        }
        if (g_strcmp0(property, "Status") == 0) {
            return g_variant_new_string("normal");
        }
        if (g_strcmp0(property, "IconThemePath") == 0) {
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
            return g_variant_builder_end(&builder);
        }
        return nullptr;
    }

    /// The dbusmenu id of the row at `index`. See the file comment on why a
    /// command id rather than a position.
    [[nodiscard]] gint32 menuIdAt(std::size_t index) const {
        const TrayMenuItem& item = items_[index];
        if (item.separator || item.id == 0) {
            return kFillerIdBase + static_cast<gint32>(index);
        }
        return static_cast<gint32>(item.id);
    }

    [[nodiscard]] const TrayMenuItem* itemForMenuId(gint32 id) const {
        for (std::size_t index = 0; index < items_.size(); ++index) {
            if (menuIdAt(index) == id) {
                return &items_[index];
            }
        }
        return nullptr;
    }

    /// One row's properties, as `a{sv}`, into an existing builder.
    void addItemProperties(GVariantBuilder& builder, const TrayMenuItem& item) const {
        if (item.separator) {
            g_variant_builder_add(&builder, "{sv}", "type",
                                  g_variant_new_string("separator"));
            return;
        }
        g_variant_builder_add(&builder, "{sv}", "label",
                              g_variant_new_string(escapeLabel(item.label).c_str()));
        g_variant_builder_add(&builder, "{sv}", "enabled",
                              g_variant_new_boolean(item.enabled ? TRUE : FALSE));
        g_variant_builder_add(&builder, "{sv}", "visible", g_variant_new_boolean(TRUE));
    }

    /// `(ia{sv}av)` for the root, or for one row when the host asked about it.
    ///
    /// The menu is one level deep, so a request for anything but the root is a
    /// request for a leaf, and a leaf's children are the empty array rather than
    /// an error.
    [[nodiscard]] GVariant* buildLayout(gint32 parentId) const {
        GVariantBuilder properties;
        g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));

        if (parentId != 0) {
            const TrayMenuItem* item = itemForMenuId(parentId);
            if (item != nullptr) {
                addItemProperties(properties, *item);
            }
            return g_variant_new("(ia{sv}av)", parentId, &properties, &children);
        }

        // Without this the panel treats the root as a command and draws nothing
        // under it.
        g_variant_builder_add(&properties, "{sv}", "children-display",
                              g_variant_new_string("submenu"));

        for (std::size_t index = 0; index < items_.size(); ++index) {
            GVariantBuilder childProperties;
            g_variant_builder_init(&childProperties, G_VARIANT_TYPE("a{sv}"));
            addItemProperties(childProperties, items_[index]);

            GVariantBuilder grandchildren;
            g_variant_builder_init(&grandchildren, G_VARIANT_TYPE("av"));

            g_variant_builder_add(&children, "v",
                                  g_variant_new("(ia{sv}av)", menuIdAt(index),
                                                &childProperties, &grandchildren));
        }
        return g_variant_new("(ia{sv}av)", 0, &properties, &children);
    }

    /// `(a(ia{sv}))` for the ids asked about, or for every row when none were.
    [[nodiscard]] GVariant* buildGroupProperties(GVariant* parameters) const {
        std::vector<gint32> wanted;
        GVariant*           ids = g_variant_get_child_value(parameters, 0);
        GVariantIter        iter;
        g_variant_iter_init(&iter, ids);
        gint32 id = 0;
        while (g_variant_iter_next(&iter, "i", &id)) {
            wanted.push_back(id);
        }
        g_variant_unref(ids);

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
        for (std::size_t index = 0; index < items_.size(); ++index) {
            const gint32 menuId = menuIdAt(index);
            // An empty id list means all of them, which is how a host asks for the
            // whole menu in one call.
            if (!wanted.empty() &&
                std::find(wanted.begin(), wanted.end(), menuId) == wanted.end()) {
                continue;
            }
            GVariantBuilder properties;
            g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
            addItemProperties(properties, items_[index]);
            g_variant_builder_add(&builder, "(ia{sv})", menuId, &properties);
        }
        return g_variant_new("(a(ia{sv}))", &builder);
    }

    /// One property of one row, as a floating variant. Never null: a host that
    /// asked about a row that has gone gets an empty string rather than an error,
    /// because the row going is a race it cannot avoid losing.
    [[nodiscard]] GVariant* propertyOf(gint32 id, const char* name) const {
        const TrayMenuItem* item = itemForMenuId(id);
        if (item == nullptr || name == nullptr) {
            return g_variant_new_string("");
        }
        if (g_strcmp0(name, "label") == 0) {
            return g_variant_new_string(escapeLabel(item->label).c_str());
        }
        if (g_strcmp0(name, "enabled") == 0) {
            return g_variant_new_boolean(item->enabled ? TRUE : FALSE);
        }
        if (g_strcmp0(name, "visible") == 0) {
            return g_variant_new_boolean(TRUE);
        }
        if (g_strcmp0(name, "type") == 0) {
            return g_variant_new_string(item->separator ? "separator" : "standard");
        }
        return g_variant_new_string("");
    }

    void handleEvent(gint32 id, const std::string& eventId) {
        // "clicked" is the only one that means anything here. The others are
        // "hovered", "opened" and "closed", which a panel sends as the pointer
        // moves and which this has nothing to do about.
        if (eventId != "clicked") {
            return;
        }
        const TrayMenuItem* item = itemForMenuId(id);
        if (item == nullptr || item->separator || !item->enabled || item->id == 0) {
            return;
        }
        publishOnUiThread(menuItemActivated, item->id);
    }

    void handleEventGroup(GVariant* parameters) {
        GVariant*    events = g_variant_get_child_value(parameters, 0);
        GVariantIter iter;
        g_variant_iter_init(&iter, events);

        gint32    id        = 0;
        gchar*    eventId   = nullptr;
        GVariant* data      = nullptr;
        guint32   timestamp = 0;
        while (g_variant_iter_next(&iter, "(isvu)", &id, &eventId, &data, &timestamp)) {
            handleEvent(id, eventId != nullptr ? eventId : "");
            g_free(eventId);
            if (data != nullptr) {
                g_variant_unref(data);
            }
        }
        g_variant_unref(events);
    }

    // --- signals -----------------------------------------------------------

    /// `parameters` is consumed, including when there is nowhere to send it.
    void emitSignal(const char* path, const char* interface, const char* name,
                    GVariant* parameters) {
        // The connection is the only thing needed. A signal is a broadcast, so
        // emitting one before the name is owned is legal and simply unheard --
        // and the host reads the whole item once it registers anyway.
        if (connection_ == nullptr) {
            if (parameters != nullptr) {
                // Or a floating reference leaks on every state change made before
                // the bus came up.
                g_variant_unref(g_variant_ref_sink(parameters));
            }
            return;
        }
        g_dbus_connection_emit_signal(connection_, nullptr, path, interface, name,
                                      parameters, nullptr);
    }

    void emitItemSignal(const char* name, GVariant* parameters) {
        emitSignal(kItemPath, kItemInterface, name, parameters);
    }

    void emitMenuSignal(const char* name, GVariant* parameters) {
        emitSignal(kMenuPath, kMenuInterface, name, parameters);
    }

    GDBusNodeInfo*   nodeInfo_   = nullptr;
    GDBusConnection* connection_ = nullptr;
    std::string      busName_;
    guint            ownerId_          = 0;
    guint            watcherId_        = 0;
    guint            itemRegistration_ = 0;
    guint            menuRegistration_ = 0;
    bool             nameOwned_        = false;
    bool             available_        = false;

    std::vector<TrayImage>    icons_;
    std::vector<TrayMenuItem> items_;
    std::string               toolTipTitle_;
    std::string               toolTipBody_;
    guint32                   revision_ = 1;
};

}  // namespace

std::unique_ptr<TrayIcon> TrayIcon::create(Dispatcher dispatch) {
    return std::make_unique<SniTrayIcon>(std::move(dispatch));
}

}  // namespace xpcog::platform
