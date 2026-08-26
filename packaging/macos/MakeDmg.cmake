# Builds the disk image.
#
#     cmake -D BUNDLE=... -D STAGE=... -D DMG=... -D VOLNAME=... \
#           -D IDENTITY=... -P MakeDmg.cmake
#
# What it produces is the plain kind: a compressed, read-only image holding
# XPCog.app and a symlink to /Applications, which is the drag-to-install gesture
# every Mac user already knows. No background picture, no window geometry, no
# AppleScript to place the icons -- all of which would mean creating a writable
# image, mounting it, driving the Finder to arrange it, unmounting and
# converting. That machinery is the single most fragile thing in a macOS release
# pipeline, it needs a logged-in session with a window server, and it buys a
# prettier window. `hdiutil create -srcfolder` needs none of it and is one call.
# ---------------------------------------------------------------------------

if(NOT BUNDLE OR NOT IS_DIRECTORY "${BUNDLE}")
    message(FATAL_ERROR "MakeDmg: no bundle at '${BUNDLE}' -- build first.")
endif()

get_filename_component(_name "${BUNDLE}" NAME)

if(NOT IDENTITY AND DEFINED ENV{XPCOG_CODESIGN_IDENTITY})
    set(IDENTITY "$ENV{XPCOG_CODESIGN_IDENTITY}")
endif()

# The staging directory is rebuilt every time rather than updated. An image is
# assembled from whatever is in this folder, so a file left behind by an earlier
# run -- a renamed helper, a bundle from before the version bump -- ships.
file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")

# ditto, not `cmake -E copy_directory`. The bundle being copied is signed by the
# time this runs, and a signature covers more than the file contents: symlinks
# have to stay symlinks, and the extended attributes and resource forks codesign
# writes have to survive the copy. ditto is the macOS tool that preserves all
# three; copy_directory follows symlinks and drops xattrs, which produces a
# bundle that fails --verify after having verified a moment earlier.
execute_process(
    COMMAND ditto "${BUNDLE}" "${STAGE}/${_name}"
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E create_symlink /Applications "${STAGE}/Applications"
    COMMAND_ERROR_IS_FATAL ANY)

file(REMOVE "${DMG}")

# UDZO is the compressed read-only format; HFS+ rather than APFS because an APFS
# image will not mount on anything older than 10.13 and gains nothing here, while
# this build's deployment target is 13.0 and an image that mounts on more than it
# has to costs nothing.
execute_process(
    COMMAND hdiutil create
            -volname "${VOLNAME}"
            -srcfolder "${STAGE}"
            -fs "HFS+"
            -format UDZO
            -ov
            -quiet
            "${DMG}"
    COMMAND_ERROR_IS_FATAL ANY)

# The image is signed too, and it is not a formality. Gatekeeper assesses the
# *container* the user downloaded, and the notarisation ticket in the next step
# is stapled to this file rather than to the bundle inside it -- so an unsigned
# image cannot be stapled, and an unstapled image asks Apple over the network at
# first launch and fails closed when the machine is offline.
if(IDENTITY)
    execute_process(
        COMMAND codesign --force --timestamp --sign "${IDENTITY}" "${DMG}"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND codesign --verify --strict --verbose=2 "${DMG}"
        COMMAND_ERROR_IS_FATAL ANY)
    set(_signed "signed as ${IDENTITY}")
else()
    set(_signed "unsigned")
endif()

file(SIZE "${DMG}" _bytes)
math(EXPR _mib "${_bytes} / 1048576")
message(STATUS "XPCog: ${DMG} -- ${_mib} MiB, ${_signed}")
