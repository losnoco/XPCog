# AdPlug, the AdLib/OPL2 player library behind roughly forty DOS-era formats.
#
# kode54's fork rather than adplug/adplug, at the commit Cog's .gitmodules pins,
# since Cog's plugin is written against that tree.
#
# ---------------------------------------------------------------------------
# Why this port carries its own CMakeLists
# ---------------------------------------------------------------------------
# The same reason libsidplayfp's does: upstream builds with autotools and
# nothing else, and running ./configure inside a vcpkg port would mean autoconf,
# automake, libtool and a POSIX shell on every platform including MSVC, to
# produce a static library from sixty-seven source files.
#
# It is a much smaller job here than it was there, and worth saying so: nothing
# under src/ includes a config.h. configure.ac probes exactly two headers
# (getopt.h and sys/io.h), and both are for the adplugdb command-line tool and
# the direct-hardware OPL backend, neither of which this port builds or needs.
# So the replacement is a source list, one generated header and two definitions.
#
# ---------------------------------------------------------------------------
# The database is not built here
# ---------------------------------------------------------------------------
# AdPlug consults a CRC-keyed database for the titles and lengths of files whose
# formats carry neither, and it ships as a separate repository (adplug/database)
# holding one 7.8 KB binary. It is not a build product and not a dependency of
# this library -- CAdPlug::set_database() takes one at run time or does without
# -- so it is carried by the codec that loads it rather than installed here.
# See codecs/adplug.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kode54/adplug
    REF 4e0141ab41ac4ebf388b765d669eb656376d04fd
    SHA512 11037987b513f4c8f22056e9dbc0c244340346e41a031ef7173bbd73250c693aa069c8b97348e8b0ee8195e8f214a9f0fc3353e3c2b5eca15dc6e625a6ef77e2
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
