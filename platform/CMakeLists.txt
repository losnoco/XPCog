# xpcog-platform is where OS-specific behaviour lives.
#
# It used to be the layer where Qt was allowed -- QtCore, QtGui and, on Linux,
# QtDBus -- so that core did not have to carry it. It now links **no UI toolkit at
# all**: the OS APIs here are Win32, C++/WinRT, CoreFoundation, MediaPlayer.framework
# and GDBus, none of which needs one, and the four public headers speak
# std::string, xpcog::Url, std::vector<std::byte> and xpcog::Signal.
#
# What that buys is substitutability. app/ can be rewritten in a different toolkit
# -- which is what happened, Qt to wx, without this directory changing at all --
# and the tests can link it without starting an application object.
#
# The one thing a toolkit was doing invisibly is thread marshalling: SMTC calls
# back on a WinRT thread and MediaPlayer.framework on a dispatch queue, and Qt's
# queued connections absorbed that. MediaIntegration::create() takes a dispatcher
# instead, so the hop is explicit and the caller decides what "the UI thread"
# means. GDBus needs none, because a GMainLoop already delivers on it.
#
# Occupants:
#   Settings       registry / CFPreferences / a file, and the paths around them
#   MediaIntegration  MPNowPlayingInfoCenter / SMTC / MPRIS
#   TaskbarIntegration ITaskbarList3 done; NSDockTile outstanding
#   FileAssociations   Windows only, by design
#   FileManager    revealing a file and moving it to the trash
#   Foreground     AllowSetForegroundWindow, Windows only
#   CrashReporter  sentry-native, and only once the listener has said yes

add_library(xpcog-platform STATIC
    src/common/FileSettingsStore.cpp
    src/common/FileSettingsStore.hpp
)

# Crash reporting. One implementation or the other is always compiled, so the
# application calls the same four functions either way and needs no #ifdef of its
# own -- the same shape as the Null* integrations below.
#
# The dependency is the reason for the switch. sentry-native builds crashpad, and
# with it a helper executable and, on Linux, libunwind; that is not something to
# impose on someone configuring by hand who only wants a player. The presets turn
# it on, exactly as they do for FFmpeg, so CI and every ordinary build have it.
if(XPCOG_WITH_SENTRY)
    find_package(sentry CONFIG REQUIRED)
    target_sources(xpcog-platform PRIVATE src/common/SentryCrashReporter.cpp)
    # PRIVATE: sentry.h appears in no header of ours. CrashReporter.hpp speaks
    # bool and string_view precisely so that the application never sees the SDK,
    # which is what lets the whole thing be swapped or removed.
    target_link_libraries(xpcog-platform PRIVATE sentry::sentry)
else()
    target_sources(xpcog-platform PRIVATE src/common/NullCrashReporter.cpp)
endif()

# Exactly one implementation of each seam is compiled, chosen here rather than by
# #ifdef inside one file. Adding a platform is a new file and a new branch, with
# no existing code to disturb.
if(WIN32)
    target_sources(xpcog-platform PRIVATE
        src/windows/WinString.hpp
        src/windows/Foreground.cpp
        src/windows/FileAssociations.cpp
        src/windows/WindowsSettings.cpp
        src/windows/WindowsFileManager.cpp
        src/common/CommonPropertyList.cpp
        src/windows/WindowsTaskbarIntegration.cpp
        src/windows/WindowsMediaIntegration.cpp)

    # Shell32 for SHChangeNotify and SHGetKnownFolderPath; Ole32 for the COM
    # apartment calls and CoTaskMemFree; Advapi32 for the registry. WindowsApp is
    # the umbrella carrying the WinRT entry points (RoGetActivationFactory and
    # friends) that C++/WinRT is built on; linking a narrower static alternative
    # is explicitly discouraged. ShCore and Shlwapi are for the synchronous COM
    # stream the artwork takes -- see WindowsMediaIntegration.cpp.
    #
    # The C++/WinRT headers themselves come from the Windows SDK's cppwinrt
    # include directory, which a developer environment already puts on INCLUDE,
    # so there is no NuGet package to restore.
    target_link_libraries(xpcog-platform PRIVATE
        Shell32.lib Ole32.lib Advapi32.lib WindowsApp.lib ShCore.lib Shlwapi.lib)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(xpcog-platform PRIVATE
        src/common/Foreground.cpp
        src/common/FileAssociations.cpp
        src/common/NullTaskbarIntegration.cpp
        src/mac/MacSettings.cpp
        src/mac/MacPropertyList.cpp
        src/mac/MacFileManager.mm
        src/mac/MacMediaIntegration.mm)
    # ARC, so the Objective-C objects there need no manual retain or release.
    set_source_files_properties(src/mac/MacMediaIntegration.mm src/mac/MacFileManager.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # CoreFoundation is for the settings store, which talks to CFPreferences
    # directly rather than through any config library -- see MacSettings.cpp for
    # why reading a real plist is the requirement.
    target_link_libraries(xpcog-platform PRIVATE
        "-framework MediaPlayer" "-framework AppKit" "-framework Foundation"
        "-framework CoreFoundation")
elseif(UNIX)
    target_sources(xpcog-platform PRIVATE
        src/common/Foreground.cpp
        src/common/FileAssociations.cpp
        src/common/NullTaskbarIntegration.cpp
        src/common/CommonSettings.cpp
        src/common/CommonPropertyList.cpp
        src/linux/GioFileManager.cpp
        src/linux/GDbusMediaIntegration.cpp)

    # MPRIS is D-Bus, and GDBus is the binding. REQUIRED rather than optional: a
    # Linux desktop without a session bus is possible, which the code handles at
    # run time, but a build environment without GLib development files is broken
    # and should say so at configure time rather than produce a player with no
    # remote control and no explanation.
    #
    # GDBus rather than libdbus-1 for one decisive reason: a GTK application's
    # main loop *is* a GMainLoop on the default main context, so an object
    # registered from the GUI thread has its method calls delivered on the GUI
    # thread. That is the guarantee QtDBus was quietly providing, and libdbus-1
    # would mean rebuilding it with a pump thread and a marshalling hop.
    #
    # From the system via pkg-config rather than as a vcpkg dependency of ours:
    # GLib is already present, both on any desktop and in vcpkg_installed, since
    # the toolkit's own port depends on GTK.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XPCOG_GIO REQUIRED IMPORTED_TARGET gio-2.0 glib-2.0 gobject-2.0)
    target_link_libraries(xpcog-platform PRIVATE PkgConfig::XPCOG_GIO)
else()
    target_sources(xpcog-platform PRIVATE
        src/common/Foreground.cpp
        src/common/FileAssociations.cpp
        src/common/NullTaskbarIntegration.cpp
        src/common/NullMediaIntegration.cpp
        src/common/NullFileManager.cpp
        src/common/CommonSettings.cpp)
endif()

add_library(XPCog::platform ALIAS xpcog-platform)

target_include_directories(xpcog-platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(xpcog-platform
    PUBLIC  xpcog-core
    PRIVATE XPCog::warnings)

set_target_properties(xpcog-platform PROPERTIES FOLDER platform)
