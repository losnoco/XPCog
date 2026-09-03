# Overrides vcpkg's community x64-osx. The five settings above the include are
# that triplet verbatim; the include is what XPCog adds. See README.md here.
#
# Nothing in CI builds this -- the macOS jobs and the DMG are arm64 -- but an
# Intel Mac configures with it, and the whole point of what the include fixes is
# that getting it wrong is silent.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/XPCogOsxTriplet.cmake")
