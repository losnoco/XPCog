# xpcog-platform is where OS-specific behaviour lives. Unlike core, Qt is allowed
# here (QtCore/QtGui/QtDBus) -- this layer exists precisely so core does not need it.
#
# Planned occupants:
#   Paths          per-OS config/data locations             (M2)
#   NowPlaying     MPNowPlayingInfoCenter / SMTC (done) / MPRIS  (M3/M5)
#   MediaKeys      MPRemoteCommandCenter / SMTC (done) / MPRIS   (M3/M5)
#   FileAccess     no-op seam replacing Cog's SandboxBroker (M2)
#   TaskbarProgress NSDockTile / ITaskbarList3              (M5)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

add_library(xpcog-platform STATIC
    src/common/Placeholder.cpp
    src/common/QSettingsStore.cpp
    # Listed so AUTOMOC sees it. It declares Q_OBJECT and has no .cpp of its own
    # -- the implementation is chosen per platform below -- and AUTOMOC only
    # looks at headers whose base name matches a listed source.
    include/xpcog/platform/MediaIntegration.hpp
)

# MediaIntegration: exactly one implementation is compiled, chosen here rather
# than by #ifdef inside one file. Adding SMTC or MPRIS is a new file and a new
# branch, with no existing code to disturb.
if(APPLE)
    enable_language(OBJCXX)
    target_sources(xpcog-platform PRIVATE src/mac/MacMediaIntegration.mm)
    # ARC, so the Objective-C objects here need no manual retain or release.
    set_source_files_properties(src/mac/MacMediaIntegration.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(xpcog-platform PRIVATE
        "-framework MediaPlayer" "-framework AppKit" "-framework Foundation")
elseif(WIN32)
    target_sources(xpcog-platform PRIVATE src/windows/WindowsMediaIntegration.cpp)
    # WindowsApp.lib is the umbrella that carries the WinRT entry points
    # (RoGetActivationFactory and friends) that C++/WinRT is built on; linking a
    # narrower static alternative is explicitly discouraged. ShCore and Shlwapi
    # are for the synchronous COM-stream route the artwork takes -- see the file.
    #
    # The C++/WinRT headers themselves come from the Windows SDK's cppwinrt
    # include directory, which a developer environment already puts on INCLUDE,
    # so there is no NuGet package to restore.
    target_link_libraries(xpcog-platform PRIVATE WindowsApp.lib ShCore.lib Shlwapi.lib)
else()
    target_sources(xpcog-platform PRIVATE src/common/NullMediaIntegration.cpp)
endif()

set_target_properties(xpcog-platform PROPERTIES AUTOMOC ON)
add_library(XPCog::platform ALIAS xpcog-platform)

target_include_directories(xpcog-platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(xpcog-platform
    PUBLIC  xpcog-core Qt6::Core Qt6::Gui
    PRIVATE XPCog::warnings)

set_target_properties(xpcog-platform PROPERTIES FOLDER platform)
