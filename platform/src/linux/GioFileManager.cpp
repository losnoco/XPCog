// Linux: GIO, which is already here for MPRIS.
//
// **Trashing** is g_file_trash(), and it implements the freedesktop.org trash
// specification properly -- the right $XDG_DATA_HOME/Trash or per-volume
// .Trash-$uid, plus the .trashinfo record without which "Restore" in the file
// manager has nowhere to put the file back. It fails rather than deletes when the
// volume has no trash, which is the behaviour this seam promises.
//
// **Revealing** is org.freedesktop.FileManager1.ShowItems, the interface Nautilus,
// Dolphin, Nemo, Thunar and PCManFM all implement precisely so that an
// application does not have to know which of them is installed. When no such
// service is on the bus -- a bare window manager, a session with no file manager
// at all -- the fallback opens the containing folder with whatever handles
// inode/directory, which is the same request minus the selection.
//
// Both are synchronous. Trashing a file is fast, and ShowItems is a bus call that
// returns as soon as the file manager has been told; neither is worth an async
// callback into a window that may have closed.

#include "xpcog/platform/FileManager.hpp"

#include <gio/gio.h>

namespace xpcog::platform {
namespace {

/// Asks the desktop's file manager to show `uri` with the item selected.
bool showItemsOverDbus(const char* uri) {
    GError*          error      = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection == nullptr) {
        g_clear_error(&error);
        return false;
    }

    const char* uris[] = {uri, nullptr};
    GVariant*   reply  = g_dbus_connection_call_sync(
        connection, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
        "org.freedesktop.FileManager1", "ShowItems", g_variant_new("(^ass)", uris, ""),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

    g_object_unref(connection);
    if (reply == nullptr) {
        g_clear_error(&error);
        return false;
    }
    g_variant_unref(reply);
    return true;
}

}  // namespace

bool revealInFileManager(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }

    GFile* file = g_file_new_for_path(path.c_str());
    gchar* uri  = g_file_get_uri(file);

    bool shown = uri != nullptr && showItemsOverDbus(uri);

    if (!shown) {
        // No FileManager1 on the bus. Open the folder instead: less than was
        // asked for, and much more than nothing happening.
        GFile* folder = g_file_get_parent(file);
        if (folder != nullptr) {
            gchar* folderUri = g_file_get_uri(folder);
            if (folderUri != nullptr) {
                GError* error = nullptr;
                shown = g_app_info_launch_default_for_uri(folderUri, nullptr, &error) != FALSE;
                g_clear_error(&error);
                g_free(folderUri);
            }
            g_object_unref(folder);
        }
    }

    g_free(uri);
    g_object_unref(file);
    return shown;
}

bool moveToTrash(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }

    GFile*  file    = g_file_new_for_path(path.c_str());
    GError* error   = nullptr;
    const bool done = g_file_trash(file, nullptr, &error) != FALSE;
    g_clear_error(&error);
    g_object_unref(file);
    return done;
}

}  // namespace xpcog::platform
