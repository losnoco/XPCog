# The macOS disk image.
#
# Three targets, none of them part of ALL and none a dependency of anything else.
# They are asked for by name, after a build, the way `installer` is on Windows:
#
#     cmake --build build/macos-app-release --target sign
#     cmake --build build/macos-app-release --target dmg
#     cmake --build build/macos-app-release --target notarize
#
# `dmg` produces build/macos-app-release/XPCog-<version>-<arch>.dmg and signs
# the bundle on the way in, so the usual sequence is `dmg` then `notarize` and
# `sign` is only wanted on its own -- to check a signature, or to run a signed
# bundle out of the build tree.
#
# --- Why not CPack ---------------------------------------------------------
#
# CPack's DragNDrop generator builds a disk image out of install() rules, and
# this project has none for the application: the bundle is assembled by CMake's
# own MACOSX_BUNDLE handling and by the post-build steps in app/CMakeLists.txt
# that stage crashpad_handler and libvgmstream. So CPack would be handed an
# empty tree, exactly as it would on Windows and for the same reason.
#
# The larger half is that a disk image is the least of what happens here. What
# has to be right is the *order* -- nested code signed before the bundle that
# contains it, the bundle signed before the image that contains it, the image
# notarised before the ticket that gets stapled to it -- and CPack has no place
# to express that other than a script it shells out to. Which is this file.
#
# --- Why three targets and not one -----------------------------------------
#
# Because notarisation is the one step that leaves the machine. It uploads the
# image to Apple, waits minutes for an answer, and needs credentials a developer
# building locally has no reason to hold. Folding it into `dmg` would make the
# ordinary case -- build an image, run it, look at it -- depend on a network
# service and a secret. Keeping it separate is also what lets CI package on
# every pull request and notarise only what it publishes.
# ---------------------------------------------------------------------------

# The tools. All four ship with macOS or with the command line tools, so this is
# a sanity check rather than a search: a Mac that cannot find codesign cannot
# have built the bundle these targets package either.
foreach(_tool codesign hdiutil ditto xcrun)
    string(TOUPPER "${_tool}" _tool_var)
    find_program(XPCOG_${_tool_var} NAMES ${_tool})
    mark_as_advanced(XPCOG_${_tool_var})
    if(NOT XPCOG_${_tool_var})
        message(STATUS
            "XPCog: ${_tool} not found -- no `dmg` target. Install the Xcode "
            "command line tools (xcode-select --install).")
        return()
    endif()
endforeach()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Worth saying out loud, for a smaller reason than the Windows one. A Debug
    # bundle is redistributable -- the macOS triplet is static and there is no
    # debug CRT to withhold -- but it is slow, it is large, and it carries
    # assertions that turn a recoverable decoding error into a dialog. Nothing
    # stops it being signed and notarised, so nothing but this line would say it
    # was the wrong tree to ship from.
    message(STATUS
        "XPCog: the `dmg` target in a Debug tree packages a debug build. "
        "Use macos-app-release to ship.")
endif()

# The identity, and it stays where it was: a cache variable set here, read at
# build time by the script, with the environment as a fallback. See the Last.fm
# block in app/CMakeLists.txt for the argument for the fallback -- it is the same
# one, and it matters more here, because a -D lands in CMakeCache.txt and this
# value is the one thing in the packaging chain that names a real certificate.
set(XPCOG_CODESIGN_IDENTITY "" CACHE STRING
    "macOS code signing identity: a \"Developer ID Application: ... (TEAMID)\" \
name, or empty for an unsigned bundle")

# Resolved by the scripts rather than here, so that this build tree can be
# configured once and signed by whoever has the certificate, without a
# reconfigure. Empty is a legitimate answer: an unsigned image is what a fork's
# pull request produces, and it is exactly what a developer without a Developer
# Program membership can build.
set(_dmg_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_OSX_ARCHITECTURES)
    string(REPLACE ";" "-" _dmg_arch "${CMAKE_OSX_ARCHITECTURES}")
endif()

set(XPCOG_DMG "${CMAKE_BINARY_DIR}/XPCog-${PROJECT_VERSION}-${_dmg_arch}.dmg"
    CACHE INTERNAL "The disk image the `dmg` target produces")

set(_scripts "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# sign
#
# Signs the bundle in place, inside out. Unlike `dmg` it insists on an identity:
# somebody who asked for this target by name and got an unsigned bundle and a
# zero exit status has been told nothing at all.
# ---------------------------------------------------------------------------
add_custom_target(sign
    COMMAND "${CMAKE_COMMAND}"
            -D "BUNDLE=$<TARGET_BUNDLE_DIR:xpcog-app>"
            -D "IDENTITY=${XPCOG_CODESIGN_IDENTITY}"
            -D "REQUIRED=ON"
            -P "${_scripts}/SignBundle.cmake"
    COMMENT "Signing XPCog.app"
    USES_TERMINAL
    VERBATIM)

# ---------------------------------------------------------------------------
# dmg
#
# Signs, then packages. Not a dependency on `sign`, because the two differ on
# what an absent identity means: here it is a warning and an unsigned image,
# there it is an error. The script takes REQUIRED=OFF and says which one it did.
# ---------------------------------------------------------------------------
add_custom_target(dmg
    COMMAND "${CMAKE_COMMAND}"
            -D "BUNDLE=$<TARGET_BUNDLE_DIR:xpcog-app>"
            -D "IDENTITY=${XPCOG_CODESIGN_IDENTITY}"
            -D "REQUIRED=OFF"
            -P "${_scripts}/SignBundle.cmake"
    COMMAND "${CMAKE_COMMAND}"
            -D "BUNDLE=$<TARGET_BUNDLE_DIR:xpcog-app>"
            -D "STAGE=${CMAKE_BINARY_DIR}/dmg-root"
            -D "DMG=${XPCOG_DMG}"
            -D "VOLNAME=XPCog ${PROJECT_VERSION}"
            -D "IDENTITY=${XPCOG_CODESIGN_IDENTITY}"
            -P "${_scripts}/MakeDmg.cmake"
    COMMENT "Building XPCog-${PROJECT_VERSION}-${_dmg_arch}.dmg"
    USES_TERMINAL
    VERBATIM)

# ---------------------------------------------------------------------------
# notarize
#
# Exists unconditionally, which is the opposite of what `installer` does on
# Windows when NSIS is missing, and deliberately so. That target is absent
# because a developer who never asked about packaging should not have a configure
# failure over a tool they do not need. This one is only ever typed on purpose,
# and a target that is missing when you type its name tells you less than a
# target that runs and says which three environment variables it wanted.
# ---------------------------------------------------------------------------
add_custom_target(notarize
    COMMAND "${CMAKE_COMMAND}"
            -D "DMG=${XPCOG_DMG}"
            -P "${_scripts}/Notarize.cmake"
    COMMENT "Notarising XPCog-${PROJECT_VERSION}-${_dmg_arch}.dmg"
    USES_TERMINAL
    VERBATIM)

# add_dependencies rather than a DEPENDS on the target name, which in
# add_custom_target means a *file* and quietly means nothing when handed a
# target. `notarize` is left out: it wants the disk image, which `dmg` produced
# in an earlier command, and rebuilding the application underneath a submission
# that is already uploading would be the wrong kind of helpful.
add_dependencies(sign xpcog-app)
add_dependencies(dmg xpcog-app)

set_target_properties(sign dmg notarize PROPERTIES FOLDER packaging)

if(XPCOG_CODESIGN_IDENTITY)
    message(STATUS "XPCog: code signing identity ${XPCOG_CODESIGN_IDENTITY}")
else()
    message(STATUS
        "XPCog: no code signing identity -- `dmg` will package an unsigned "
        "bundle. Set XPCOG_CODESIGN_IDENTITY, in the cache or the environment.")
endif()
