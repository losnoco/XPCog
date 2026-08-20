# libbinio, which exists here only because AdPlug reads through it.
#
# At the commit Cog's .gitmodules pins, from adplug/libbinio rather than a
# release tarball: the last tagged release is 1.5 and the tree has moved nine
# commits past it, which is what AdPlug is written against.
#
# ---------------------------------------------------------------------------
# Why this port carries its own CMakeLists
# ---------------------------------------------------------------------------
# Upstream ships one, and it cannot be used. Two reasons, either of which alone
# would be enough:
#
#   * It installs nothing. There is no install() anywhere in the tree, so a
#     vcpkg build produces a library in a build directory and then deletes it.
#
#   * Its configure_file() writes binio.h into ${CMAKE_SOURCE_DIR}/src -- into
#     the *source* tree, not the build tree. vcpkg builds a read-only extracted
#     source with separate release and debug trees, so that is either a
#     permission error or two configurations racing to write one file.
#
# The replacement does the same four type probes, generates binio.h into the
# build tree, and installs. Nothing about the library's behaviour changes.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO adplug/libbinio
    REF e2f8d50c53102c618d675c3310e09a0e0bdf49cd
    SHA512 834bebc29f489c37d238a68180e23288e4b59cee5376752d2fb6f5e509f06185f26a942ecac8e5939eda8408d72f3ac5b9e472420a64eed15669e1e61400ccf3
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_copy_pdbs()

# Headers ship once; vcpkg rejects a debug tree carrying its own copy.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
