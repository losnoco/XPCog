# Overrides vcpkg's built-in arm64-osx. The five settings above the include are
# that triplet verbatim; the include is what XPCog adds. See README.md here.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/XPCogOsxTriplet.cmake")
