// The little that stands between C++ and GObject in this frontend.
//
// GTK is a C library and this is C++20; the join is a handful of RAII types and
// one trampoline, all here, so nothing else in app-gtk/ writes g_object_unref,
// g_free or a static callback by hand. Deliberately no gtkmm: the C API is what
// GNOME's own documentation describes, libadwaita's C++ wrapper is packaged by
// no distribution this targets, and the seam these helpers cover is small.
//
// Ownership, because GObject has three kinds and gets one of them wrong for
// C++'s taste:
//
//   full reference     what g_object_new and most *_new constructors return for
//                      a plain GObject. GObjectPtr::adopt() takes it.
//   floating reference what every GtkWidget starts life with (GInitiallyUnowned
//                      is still the base class in GTK 4). The first container it
//                      is put in sinks it; a widget nobody parents leaks or,
//                      worse, is sunk by something that then unrefs it. sink()
//                      turns one into a full reference this object owns.
//   borrowed           what gtk_builder_get_object and every getter returns.
//                      ref() takes a reference of its own; a raw pointer is
//                      fine for the length of a call.
//
// Widgets in this frontend are almost always borrowed: the builder owns the
// tree until the window is realised and the window owns it afterwards, and a
// C++ object that outlives the window holds nothing it could dangle on.

#pragma once

#include <glib-object.h>
#include <glib.h>

#include <functional>
#include <memory>
#include <type_traits>
#include <string>
#include <utility>

namespace xpcog::gtk {

/// An owning reference to a GObject.
///
/// GObjects only: it releases with g_object_unref(), and handed a boxed type
/// that call reads the wrong memory as a class pointer and crashes. The C
/// types are incomplete here, so derivation cannot be checked; the boxed
/// types this frontend actually touches are refused by name instead, and
/// have owners of their own below.
template <typename T>
class GObjectPtr {
    static_assert(!std::is_same_v<T, GBytes> && !std::is_same_v<T, GVariant> &&
                      !std::is_same_v<T, GError> && !std::is_same_v<T, GHashTable> &&
                      !std::is_same_v<T, GKeyFile> && !std::is_same_v<T, GMainContext>,
                  "not a GObject: use its own owner (GBytesPtr, GErrorPtr, ...)");

public:
    GObjectPtr() = default;

    /// Takes over a full reference.
    static GObjectPtr adopt(T* object) { return GObjectPtr(object); }

    /// Takes a reference of its own on a borrowed object.
    static GObjectPtr ref(T* object) {
        if (object) g_object_ref(object);
        return GObjectPtr(object);
    }

    /// Sinks a floating reference and owns the result. For a freshly built
    /// widget that is not about to be parented.
    static GObjectPtr sink(T* object) {
        if (object) g_object_ref_sink(object);
        return GObjectPtr(object);
    }

    GObjectPtr(const GObjectPtr& other) : object_(other.object_) {
        if (object_) g_object_ref(object_);
    }
    GObjectPtr(GObjectPtr&& other) noexcept
        : object_(std::exchange(other.object_, nullptr)) {}

    GObjectPtr& operator=(GObjectPtr other) noexcept {
        std::swap(object_, other.object_);
        return *this;
    }

    ~GObjectPtr() { reset(); }

    void reset() {
        if (object_) g_object_unref(object_);
        object_ = nullptr;
    }

    [[nodiscard]] T* get() const { return object_; }
    [[nodiscard]] T* release() { return std::exchange(object_, nullptr); }
    explicit operator bool() const { return object_ != nullptr; }

private:
    explicit GObjectPtr(T* object) : object_(object) {}

    T* object_ = nullptr;
};

/// An owning reference to a GBytes, which is boxed, not a GObject.
class GBytesPtr {
public:
    GBytesPtr() = default;
    /// Takes over a full reference.
    explicit GBytesPtr(GBytes* bytes) : bytes_(bytes) {}
    GBytesPtr(const GBytesPtr&)            = delete;
    GBytesPtr& operator=(const GBytesPtr&) = delete;
    GBytesPtr(GBytesPtr&& other) noexcept : bytes_(std::exchange(other.bytes_, nullptr)) {}
    GBytesPtr& operator=(GBytesPtr&& other) noexcept {
        std::swap(bytes_, other.bytes_);
        return *this;
    }
    ~GBytesPtr() {
        if (bytes_) g_bytes_unref(bytes_);
    }

    [[nodiscard]] GBytes* get() const { return bytes_; }

private:
    GBytes* bytes_ = nullptr;
};

/// A string GLib allocated and this side must free.
struct GStr {
    GStr() = default;
    explicit GStr(char* s) : value(s) {}
    GStr(const GStr&) = delete;
    GStr& operator=(const GStr&) = delete;
    GStr(GStr&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    GStr& operator=(GStr&& other) noexcept {
        if (this != &other) {
            g_free(value);
            value = std::exchange(other.value, nullptr);
        }
        return *this;
    }
    ~GStr() { g_free(value); }

    /// The text, or empty for a null answer -- GLib returns null for "none"
    /// more often than it returns "".
    [[nodiscard]] std::string str() const { return value ? value : ""; }
    [[nodiscard]] const char* c_str() const { return value ? value : ""; }
    explicit operator bool() const { return value != nullptr; }

    char* value = nullptr;
};

/// A GError slot: pass `&error.value` to a GLib call and read it afterwards.
struct GErrorPtr {
    GErrorPtr() = default;
    GErrorPtr(const GErrorPtr&) = delete;
    GErrorPtr& operator=(const GErrorPtr&) = delete;
    ~GErrorPtr() { g_clear_error(&value); }

    [[nodiscard]] std::string message() const {
        return value && value->message ? value->message : "";
    }
    explicit operator bool() const { return value != nullptr; }

    GError* value = nullptr;
};

// --- Signals -----------------------------------------------------------------
//
// connect<Signature>(instance, "name", callable) attaches a C++ callable to a
// GObject signal. Signature is the C handler's, *including the instance as its
// first parameter and excluding user data*, which is how GObject documents every
// signal: `void(GtkButton*)` for "clicked", `gboolean(GtkWindow*)` for
// "close-request". The callable is copied to the heap and freed with the
// connection, so a lambda may capture what it likes as long as what it captures
// outlives the object it is connected to -- or the Connection below is used to
// cut the tie first.

namespace detail {

template <typename Signature>
struct Trampoline;

template <typename R, typename... Args>
struct Trampoline<R(Args...)> {
    using Fn = std::function<R(Args...)>;

    static R call(Args... args, gpointer data) {
        return (*static_cast<Fn*>(data))(args...);
    }

    static void destroy(gpointer data, GClosure*) { delete static_cast<Fn*>(data); }
};

}  // namespace detail

template <typename Signature, typename F>
gulong connect(gpointer instance, const char* signal, F&& callable) {
    using T = detail::Trampoline<Signature>;
    auto* fn = new typename T::Fn(std::forward<F>(callable));
    return g_signal_connect_data(instance, signal, G_CALLBACK(&T::call), fn,
                                 &T::destroy, static_cast<GConnectFlags>(0));
}

/// A signal connection that is severed when this object goes away, if the
/// emitter is still alive to sever it from. The instance is watched with a weak
/// pointer, so an emitter destroyed first costs nothing and crashes nothing.
class Connection {
public:
    Connection() = default;

    Connection(gpointer instance, gulong handler)
        : instance_(instance), handler_(handler) {
        if (instance_) g_object_add_weak_pointer(G_OBJECT(instance_), &instance_);
    }

    template <typename Signature, typename F>
    static Connection to(gpointer instance, const char* signal, F&& callable) {
        return Connection(instance,
                          connect<Signature>(instance, signal, std::forward<F>(callable)));
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept { *this = std::move(other); }

    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            disconnect();
            instance_ = other.instance_;
            handler_ = other.handler_;
            if (instance_) {
                g_object_remove_weak_pointer(G_OBJECT(instance_), &other.instance_);
                g_object_add_weak_pointer(G_OBJECT(instance_), &instance_);
            }
            other.instance_ = nullptr;
            other.handler_ = 0;
        }
        return *this;
    }

    ~Connection() { disconnect(); }

    void disconnect() {
        if (instance_) {
            if (handler_ && g_signal_handler_is_connected(instance_, handler_)) {
                g_signal_handler_disconnect(instance_, handler_);
            }
            g_object_remove_weak_pointer(G_OBJECT(instance_), &instance_);
        }
        instance_ = nullptr;
        handler_ = 0;
    }

private:
    gpointer instance_ = nullptr;
    gulong handler_ = 0;
};

// --- Main-loop sources -------------------------------------------------------

/// A repeating timeout on the default main context, removed when this object
/// goes away. The callable returns nothing; the source stays until stop().
class Timeout {
public:
    Timeout() = default;
    Timeout(const Timeout&) = delete;
    Timeout& operator=(const Timeout&) = delete;
    ~Timeout() { stop(); }

    void start(unsigned intervalMs, std::function<void()> callable) {
        stop();
        callable_ = std::make_unique<std::function<void()>>(std::move(callable));
        source_ = g_timeout_add_full(G_PRIORITY_DEFAULT, intervalMs, &Timeout::fire,
                                     callable_.get(), nullptr);
    }

    void stop() {
        if (source_) g_source_remove(source_);
        source_ = 0;
        callable_.reset();
    }

    [[nodiscard]] bool running() const { return source_ != 0; }

private:
    static gboolean fire(gpointer data) {
        (*static_cast<std::function<void()>*>(data))();
        return G_SOURCE_CONTINUE;
    }

    guint source_ = 0;
    std::unique_ptr<std::function<void()>> callable_;
};

/// Run a callable once on the default main context, from any thread. This is
/// what the frontend hands to everything in core that takes an
/// xpcog::Dispatcher: the default context is GTK's, so the callable runs on the
/// interface thread, and platform/'s GDBus objects were written on the
/// assumption that somebody iterates that context -- here, GTK itself.
inline void postToMainContext(std::function<void()> callable) {
    auto* fn = new std::function<void()>(std::move(callable));
    g_main_context_invoke_full(
        nullptr, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        },
        fn, [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

}  // namespace xpcog::gtk
