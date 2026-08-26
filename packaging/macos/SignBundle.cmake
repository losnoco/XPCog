# Signs XPCog.app, inside out.
#
#     cmake -D BUNDLE=... -D IDENTITY=... -D REQUIRED=ON|OFF -P SignBundle.cmake
#
# --- Why not --deep --------------------------------------------------------
#
# The target this replaces was one line: `codesign --force --deep --options
# runtime --timestamp --sign <identity> XPCog.app`. It produced a bundle that
# passed `codesign --verify` and notarised, so what follows is not a bug report.
#
# --deep signs the outer bundle and then walks inward applying the *same*
# identity, the same options and the same (absent) entitlements to everything it
# finds. Apple's own guidance calls it unsuitable for signing nested code, and
# the reasons are all reasons about a bundle that has more in it than this one
# does today: a helper with its own entitlements gets the outer bundle's, a
# nested component that was already correctly signed by someone else is
# overwritten, and anything --deep fails to recognise as code is silently left
# alone. It is a repair tool for a bundle whose insides are already signed.
#
# Signing inside out is what the tool actually asks for, and it is four lines
# rather than one. The order is the whole content of it: a nested signature is
# part of the bytes the enclosing signature covers, so signing the bundle first
# and libvgmstream second produces a bundle whose seal no longer matches its
# contents -- which is a failure at first launch on someone else's Mac, long
# after this script exited zero.
#
# --- Why no entitlements file ----------------------------------------------
#
# There is none, and that is the intended state rather than an omission. An
# entitlement is a hole in the hardened runtime, and this program wants no holes:
# it is not sandboxed, so reading the user's music needs no file-access
# entitlement; it records nothing, so it needs no microphone; nothing here JITs;
# and every library in the bundle is signed by this same identity, so library
# validation has nothing to disable. Notarisation requires the hardened runtime,
# which `--options runtime` turns on, and nothing beyond it.
#
# The three that would arrive if that stopped being true, so the next person does
# not have to work out which: com.apple.security.cs.allow-jit for a dynarec in
# one of the emulator cores -- mGBA has one upstream and this build does not
# enable it; com.apple.security.cs.disable-library-validation to load a plugin
# signed by somebody else, which the compile-time codec registry means will not
# happen; and com.apple.security.device.audio-input if XPCog ever records.
# ---------------------------------------------------------------------------

if(NOT BUNDLE OR NOT IS_DIRECTORY "${BUNDLE}")
    message(FATAL_ERROR "SignBundle: no bundle at '${BUNDLE}' -- build first.")
endif()

# The environment as a fallback, so that a build tree configured by one person
# can be signed by another without a reconfigure, and so that CI can supply the
# identity without it landing in CMakeCache.txt. See packaging/macos/CMakeLists.txt.
if(NOT IDENTITY AND DEFINED ENV{XPCOG_CODESIGN_IDENTITY})
    set(IDENTITY "$ENV{XPCOG_CODESIGN_IDENTITY}")
endif()

if(NOT IDENTITY)
    if(REQUIRED)
        message(FATAL_ERROR
            "XPCog: no code signing identity.\n"
            "Set XPCOG_CODESIGN_IDENTITY to a \"Developer ID Application: NAME "
            "(TEAMID)\" name, either in the cache or in the environment. "
            "`security find-identity -v -p codesigning` lists the ones this "
            "machine holds.")
    endif()
    # The other caller is `dmg`, where an unsigned bundle is a legitimate
    # outcome -- a fork's pull request has no certificate and should still
    # produce an image that proves the packaging works. Said out loud because an
    # unsigned image is refused by Gatekeeper everywhere but here, and that is
    # not something to discover from a download.
    message(STATUS
        "XPCog: no code signing identity -- leaving the bundle unsigned. "
        "Gatekeeper will refuse it on any Mac but this one.")
    return()
endif()

get_filename_component(_name "${BUNDLE}" NAME_WE)
set(_main "${BUNDLE}/Contents/MacOS/${_name}")

# Everything the enclosing signature will cover, deepest first.
#
# Two directories and not a recursive walk of the bundle: Frameworks is where
# app/CMakeLists.txt stages libvgmstream, MacOS is where it stages
# crashpad_handler, and those are the only two places a Mach-O file can be in a
# bundle laid out correctly. A recursive walk would additionally find the
# SoundFonts and the equaliser presets in Resources, which are data and which
# codesign would refuse -- and refusing them one at a time is a worse way to
# learn the layout than knowing it here.
file(GLOB _nested
    "${BUNDLE}/Contents/Frameworks/*"
    "${BUNDLE}/Contents/MacOS/*")
list(REMOVE_ITEM _nested "${_main}")
list(SORT _nested)

foreach(_item IN LISTS _nested)
    message(STATUS "XPCog: signing ${_item}")
    execute_process(
        COMMAND codesign --force --options runtime --timestamp
                --sign "${IDENTITY}" "${_item}"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()

message(STATUS "XPCog: signing ${BUNDLE}")
execute_process(
    COMMAND codesign --force --options runtime --timestamp
            --sign "${IDENTITY}" "${BUNDLE}"
    COMMAND_ERROR_IS_FATAL ANY)

# Verified here rather than trusted, because every way this goes wrong goes wrong
# quietly. --strict is what refuses a bundle whose contents no longer match its
# seal, and --deep on *verification* is exactly right where it was wrong on
# signing: checking every nested signature is the point.
execute_process(
    COMMAND codesign --verify --deep --strict --verbose=2 "${BUNDLE}"
    COMMAND_ERROR_IS_FATAL ANY)

# The line CI reads. Asserting on a step's own account of what it did beats
# asserting on the secret being non-empty, for the reason the Last.fm check in
# .github/workflows/ci.yml gives at length.
message(STATUS "XPCog: signed ${_name}.app as ${IDENTITY}")
