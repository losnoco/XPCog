# The one shared library XPCog installs beside itself on Linux, and the rpath
# that lets an installed executable find it.
#
# --- Why there is anything to do here at all -------------------------------
#
# Every other dependency is either the distribution's -- found by the dynamic
# loader in the ordinary way, with no help from us -- or statically linked. One
# is neither. ports/vgmstream forces BUILD_SHARED_LIBS because only vgmstream's
# *shared* target carries install rules and a CMake export; the static library it
# also builds is for its own CLI and is never installed. So vcpkg hands back
# libvgmstream.so whatever the triplet asked for.
#
# In the build tree that library is found through a RUNPATH pointing into
# vcpkg_installed, which CMake writes for exactly this reason. `cmake --install`
# then strips it -- correctly, because it names a directory that will not exist
# on the target machine -- and the installed binary stops starting at all:
#
#     error while loading shared libraries: libvgmstream.so:
#     cannot open shared object file: No such file or directory
#
# So the library is installed into libdir and the executables get an rpath that
# reaches it relative to themselves. $ORIGIN is the loader's own spelling of
# "the directory this executable is in", so the answer survives the whole tree
# being moved or installed under any prefix -- which a DESTDIR-staged package,
# a tarball unpacked into /opt, and a Flatpak's /app all need.
#
# This is the same problem app/CMakeLists.txt solves for the macOS bundle with
# @executable_path/../Frameworks, and it is solved the same way, one loader's
# vocabulary at a time.

# Arrange for `target` to find XPCog's bundled runtime libraries once installed.
#
# A function rather than two copies because both installed executables need it
# and they are declared in different directories: xpcog-app in app/, xpcog-cli in
# tools/cli/, and a headless build installs the second without the first.
function(xpcog_install_linux_runtime target)
    if(NOT UNIX OR APPLE)
        return()
    endif()

    # Relative to the executable, not to a prefix baked in at configure time.
    set_target_properties(${target} PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")

    # And the library itself, once, however many targets ask. install() is not
    # idempotent -- two calls naming the same file copy it twice and, worse, put
    # two identical entries in an install manifest a package builder reads.
    if(TARGET vgmstream::vgmstream)
        get_property(_done GLOBAL PROPERTY XPCOG_LINUX_RUNTIME_INSTALLED)
        if(NOT _done)
            # IMPORTED_RUNTIME_ARTIFACTS rather than install(FILES) of a
            # $<TARGET_FILE:>: it is the form that knows an imported shared
            # library is a runtime artefact, and it follows the port's own
            # notion of which file that is rather than this file guessing.
            install(IMPORTED_RUNTIME_ARTIFACTS vgmstream::vgmstream
                    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
            set_property(GLOBAL PROPERTY XPCOG_LINUX_RUNTIME_INSTALLED ON)
        endif()
    endif()
endfunction()
