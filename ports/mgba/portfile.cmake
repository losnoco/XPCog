# mGBA, as the GBA core behind GSF.
#
# An overlay port rather than a submodule, though Cog carries it as one. The
# rule in ../README.md is that anything with a real upstream and a pinned commit
# becomes a port, and here that is also the cheaper answer: mGBA is 989 sources,
# and CI binary-caches ports, so it compiles once per platform instead of on
# every push. A submodule would put 77 MB in the tree and rebuild every time.
#
# kode54's fork rather than mgba/mgba, at the commit Cog's .gitmodules pins.
# HighlyComplete is written against this tree, and the fork's tip is an MSVC fix.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kode54/mgba
    REF f6b1854c373fd7cdf8571b9d8568f68bc2decdb1
    SHA512 747059946ddd313da1d74742137ff9717ab198d1661aed367c34f64a63c7dd90e2fa9b519ea5b6551b4f30b6d16a6cc3e120cd3442de41e6d80c9f37fff76d95
    HEAD_REF master
)

# Not LIBMGBA_ONLY, which is mGBA's own switch for "library only" and is the
# obvious thing to reach for. It forces DISABLE_DEPS, DISABLE_DEPS forces
# USE_ZLIB off, and mGBA without zlib compiles its own crc32() -- with zlib's
# exact signature. XPCog links the real zlib for psflib, lazyusf2 and libarchive,
# so the two collide at link time:
#
#     duplicate symbol '_crc32' in: libz.a(crc32.c.o), libmgba.a(crc32.c.o)
#
# Both of those `set()` calls are plain rather than cached, so -DUSE_ZLIB=ON on
# the command line cannot win. Hence the list below: it is what LIBMGBA_ONLY
# expands to, minus DISABLE_DEPS, so mGBA shares the one zlib in the process
# instead of bringing a second.
#
# DISABLE_FRONTENDS is the part that matters for build time -- without it mGBA's
# CMake goes looking for Qt and SDL and builds two players. The rest is stated
# rather than left to default because a default that changes upstream would
# reintroduce a dependency silently.
#
# GBA only. A GSF is a Game Boy Advance program by definition, and Cog's Xcode
# project likewise defines M_CORE_GBA without M_CORE_GB.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DDISABLE_FRONTENDS=ON
        -DBUILD_STATIC=ON
        -DBUILD_SHARED=OFF
        -DM_CORE_GBA=ON
        -DM_CORE_GB=OFF
        -DUSE_ZLIB=ON
        -DUSE_MINIZIP=OFF
        -DUSE_PNG=OFF
        -DUSE_LIBZIP=OFF
        -DUSE_SQLITE3=OFF
        -DUSE_ELF=OFF
        -DUSE_LUA=OFF
        -DUSE_JSON_C=OFF
        -DUSE_FREETYPE=OFF
        -DUSE_LZMA=OFF
        -DUSE_FFMPEG=OFF
        -DUSE_DISCORD_RPC=OFF
        -DUSE_EPOXY=OFF
        -DUSE_EDITLINE=OFF
        -DENABLE_SCRIPTING=OFF
        -DENABLE_DEBUGGERS=OFF
        -DENABLE_GDB_STUB=OFF
        -DBUILD_GL=OFF
        -DBUILD_GLES2=OFF
        -DBUILD_GLES3=OFF
        -DBUILD_QT=OFF
        -DBUILD_SDL=OFF
        -DBUILD_LIBRETRO=OFF
        -DBUILD_PERF=OFF
        -DBUILD_TEST=OFF
        -DBUILD_SUITE=OFF
        -DBUILD_HEADLESS=OFF
        -DBUILD_EXAMPLE=OFF
        -DBUILD_PYTHON=OFF
        -DBUILD_DOCGEN=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

# Headers and CMake/pkg-config bits ship once; vcpkg rejects a debug tree that
# carries its own copy of either.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
                    "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
