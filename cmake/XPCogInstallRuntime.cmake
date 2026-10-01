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
# This is the problem a macOS bundle solves with @executable_path/../Frameworks,
# solved the same way, in this loader's vocabulary.

# Arrange for `target` to find XPCog's bundled runtime libraries once installed.
#
# A function rather than two copies because both installed executables need it
# and they are declared in different directories: xpcog-gtk in app-gtk/, xpcog-cli in
# tools/cli/, and a headless build installs the second without the first.
function(xpcog_install_linux_runtime target)
    if(NOT UNIX)
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

# crashpad's handler, staged beside a player's executable.
#
# crashpad is out-of-process by design -- that is the whole reason it can report
# a crash that has already taken the player's own address space with it -- so it
# ships as a second executable that has to sit somewhere the first one can find.
# sentry-native sets *no* default for its path, and the failure without it is
# the quiet kind: sentry_init() succeeds, messages are still sent, and actual
# crashes simply never appear. So it is staged beside the executable, which is
# where SentryCrashReporter.cpp looks, and taken from vcpkg's tools/ directory
# by name rather than found on PATH: what has to run is the handler from the
# same sentry-native the player linked against.
#
# With the DLLs vcpkg keeps beside it, not only the .exe. vcpkg builds its tools
# in release whatever the tree is, so the handler links the release zlib, z.dll;
# a debug tree's bin/ holds only zd.dll, which applocal copied for the player.
# Staged alone, the handler then fails to start at the first launch with crash
# reporting consented, as a loader error box from a process nobody started. In a
# release tree z.dll is already there and the copy changes nothing.
function(xpcog_stage_crashpad target)
    if(NOT XPCOG_WITH_SENTRY OR NOT WIN32)
        return()
    endif()
    set(_tools "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/sentry-native")
    set(_handler "${_tools}/crashpad_handler${CMAKE_EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${_handler}")
        message(STATUS
            "XPCog: crashpad_handler not found at ${_handler} -- this build will "
            "report messages but not crashes.")
        return()
    endif()
    file(GLOB _dlls "${_tools}/*.dll")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_handler}" ${_dlls} "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Staging crashpad_handler"
        VERBATIM)
endfunction()
