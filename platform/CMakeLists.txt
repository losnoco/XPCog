# xpcog-platform is where OS-specific behaviour lives. Unlike core, Qt is allowed
# here (QtCore/QtGui/QtDBus) -- this layer exists precisely so core does not need it.
#
# Planned occupants:
#   Paths          per-OS config/data locations             (M2)
#   NowPlaying     MPNowPlayingInfoCenter / SMTC / MPRIS    (M3/M5)
#   MediaKeys      MPRemoteCommandCenter / SMTC / MPRIS     (M3/M5)
#   FileAccess     no-op seam replacing Cog's SandboxBroker (M2)
#   TaskbarProgress NSDockTile / ITaskbarList3              (M5)

find_package(Qt6 REQUIRED COMPONENTS Core)

add_library(xpcog-platform STATIC src/common/Placeholder.cpp)
add_library(XPCog::platform ALIAS xpcog-platform)

target_include_directories(xpcog-platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(xpcog-platform
    PUBLIC  xpcog-core Qt6::Core
    PRIVATE XPCog::warnings)

set_target_properties(xpcog-platform PROPERTIES FOLDER platform)
