# The Windows installer.
#
# Not part of ALL, and not a dependency of anything: `installer` is asked for by
# name, after a build, the same way `sign` is on macOS.
#
#     cmake --build build/windows-release --target installer
#
# It produces build/windows-release/XPCog-<version>-x64-setup.exe.
#
# --- Why not CPack ---------------------------------------------------------
#
# CPack has an NSIS generator, and it packages what install() rules say to
# package. This project has no install() rules for the application on Windows,
# for the reason Harvest.cmake sets out: the deployed folder is assembled by
# vcpkg's applocal post-build step, not by CMake, so there is nothing for CPack
# to read. Everything past that -- the install-mode page, running the app's own
# --register, refusing to overwrite a running copy -- would be
# CPACK_NSIS_EXTRA_INSTALL_COMMANDS strings pasted into a template this file
# cannot see, which is a worse place to keep NSIS than an NSIS script.

find_program(XPCOG_MAKENSIS
    NAMES makensis
    HINTS
        # negrutiu's fork before the official build, for the reason the probe
        # below sets out: it is the one that can emit a 64-bit installer. Its
        # own installer offers NSIS_FORK as the directory precisely so that it
        # can sit beside an official copy, so a machine with both keeps both and
        # this picks the more capable one.
        "$ENV{PROGRAMFILES}/NSIS_FORK"
        "$ENV{PROGRAMW6432}/NSIS_FORK"
        "$ENV{ProgramFiles\(x86\)}/NSIS_FORK"
        "$ENV{PROGRAMFILES}/NSIS"
        "$ENV{ProgramFiles\(x86\)}/NSIS"
        "$ENV{PROGRAMW6432}/NSIS"
    DOC "NSIS compiler, for the `installer` target")
mark_as_advanced(XPCOG_MAKENSIS)

if(NOT XPCOG_MAKENSIS)
    # A status line rather than a fatal error: NSIS is needed to *package* XPCog
    # and not to build or run it, so a developer without it should get a build
    # that works and one absent target, not a configure failure.
    message(STATUS
        "XPCog: makensis not found -- no `installer` target. "
        "Install NSIS (https://nsis.sourceforge.io) to build one.")
    return()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Worth saying out loud. A Debug build links the debug CRT and the debug wx
    # DLLs, neither of which is redistributable, and an installer built from one
    # fails on any machine without Visual Studio -- as a missing-DLL dialog, long
    # after the point where it could have been explained.
    message(STATUS
        "XPCog: the `installer` target in a Debug tree packages debug runtime "
        "DLLs, which cannot be redistributed. Use windows-release to ship.")
endif()

# A 32-bit or a 64-bit installer, decided by asking this compiler rather than by
# being told which one it is.
#
# Every official NSIS emits a 32-bit installer and nothing else. The `Target`
# directive exists there -- 3.07 and later -- but the stubs beside it are
# x86-ansi and x86-unicode, so `Target amd64-unicode` is a compile error rather
# than a different output. negrutiu's fork (https://github.com/negrutiu/nsis)
# ships amd64-unicode stubs and the matching plugins, and changes nothing else:
# the script below is the same script either way.
#
# What a 64-bit installer buys, given that a 32-bit one installs a 64-bit
# program perfectly well and almost every Windows application ships exactly
# that: it stands on the same side of WoW64 as the program it installs. The
# registry view is the case that matters and the script says so at SetRegView --
# a 32-bit installer writing HKLM\Software lands in WOW6432Node while XPCog.exe
# reads the 64-bit view. That is fixable from the script, and is fixed there, but
# the whole class of it stops existing when the stub is native.
#
# What it costs is one message. A 32-bit Windows cannot start a 64-bit installer
# at all, so instead of this script's "XPCog requires 64-bit Windows" the user
# gets the shell's "This app can't run on your PC". The triplet is x64-windows;
# there was never going to be anything to install on that machine.
#
# Probed by compiling rather than by reading a version string, because what
# matters is whether the amd64 stubs are installed and the version does not say.
# The answer is cached against the path it was measured for, so a reconfigure
# does not pay for it and changing XPCOG_MAKENSIS does.
if(NOT XPCOG_NSIS_AMD64_PROBED STREQUAL XPCOG_MAKENSIS)
    set(_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/amd64-probe")
    file(MAKE_DIRECTORY "${_probe_dir}")
    file(WRITE "${_probe_dir}/probe.nsi"
        "Target amd64-unicode\n"
        "OutFile \"probe.exe\"\n"
        "RequestExecutionLevel user\n"
        "Section\n"
        "SectionEnd\n")
    execute_process(
        COMMAND "${XPCOG_MAKENSIS}" -V1 "probe.nsi"
        WORKING_DIRECTORY "${_probe_dir}"
        RESULT_VARIABLE _probe_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(_probe_result EQUAL 0)
        set(XPCOG_NSIS_AMD64 ON CACHE INTERNAL "makensis can emit a 64-bit installer")
    else()
        set(XPCOG_NSIS_AMD64 OFF CACHE INTERNAL "makensis can emit a 64-bit installer")
    endif()
    set(XPCOG_NSIS_AMD64_PROBED "${XPCOG_MAKENSIS}" CACHE INTERNAL
        "the makensis XPCOG_NSIS_AMD64 was measured for")
    file(REMOVE_RECURSE "${_probe_dir}")
endif()

if(XPCOG_NSIS_AMD64)
    set(XPCOG_NSIS_TARGET "Target amd64-unicode")
    message(STATUS "XPCog: NSIS at ${XPCOG_MAKENSIS} -- 64-bit installer")
else()
    # Unicode true is what the script carried before there was a choice, and is
    # what `Target x86-unicode` means; spelled the old way so that this line
    # also compiles under an NSIS older than 3.07.
    set(XPCOG_NSIS_TARGET "Unicode true")
    message(STATUS
        "XPCog: NSIS at ${XPCOG_MAKENSIS} -- 32-bit installer. "
        "negrutiu's NSIS fork builds a 64-bit one; see packaging/windows/CMakeLists.txt.")
endif()

# NSIS wants four integers; the project has three.
set(XPCOG_VERSION "${PROJECT_VERSION}")
set(XPCOG_VERSION_4
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")

set(XPCOG_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${XPCOG_GENERATED_DIR}")

set(XPCOG_INSTALLER_OUTPUT
    "${CMAKE_BINARY_DIR}/XPCog-${PROJECT_VERSION}-x64-setup.exe")

# Native separators for everything that reaches the script, and not as a style
# choice: makensis's !include resolves backslash paths only. Handed
# "C:/a/b/payload-install.nsh" it reports "could not find" for a file that is
# plainly there, which reads as a harvest step that failed rather than as a path
# it declined to parse. Converting all four keeps one rule instead of a
# per-directive one -- and a backslash inside a quoted NSIS string is inert,
# since NSIS escapes with $\ rather than with \.
#
# The CMake-form variables stay, because the other two consumers -- file() here
# and the -D on the harvest command line -- want forward slashes.
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/app/icons/xpcog.ico" XPCOG_ICON)
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/COPYING" XPCOG_LICENSE_FILE)
file(TO_NATIVE_PATH "${XPCOG_GENERATED_DIR}" XPCOG_GENERATED_DIR_NATIVE)
file(TO_NATIVE_PATH "${XPCOG_INSTALLER_OUTPUT}" XPCOG_INSTALLER_OUTPUT_NATIVE)

set(XPCOG_NSI "${CMAKE_CURRENT_BINARY_DIR}/XPCog.nsi")
configure_file(XPCog.nsi.in "${XPCOG_NSI}" @ONLY)

add_custom_target(installer
    # Harvest first: the file lists the script !includes describe what this build
    # produced, so they have to be regenerated every time rather than cached.
    COMMAND "${CMAKE_COMMAND}"
            -D "XPCOG_STAGE_DIR=$<TARGET_FILE_DIR:xpcog-app>"
            -D "XPCOG_OUT_DIR=${XPCOG_GENERATED_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Harvest.cmake"
    # -V2 rather than /V2: makensis accepts both on Windows, and only the dash
    # form survives being typed into a POSIX-style shell, where /V2 is rewritten
    # into a path by MSYS and reported as a missing script.
    COMMAND "${XPCOG_MAKENSIS}" -V2 "${XPCOG_NSI}"
    COMMAND "${CMAKE_COMMAND}" -E echo "Installer: ${XPCOG_INSTALLER_OUTPUT}"
    DEPENDS "${XPCOG_NSI}"
    COMMENT "Building the Windows installer"
    VERBATIM
    USES_TERMINAL)

# The app, and the assets it is packaged with -- xpcog-assets is an ALL target so
# a plain build already staged them, but `--target installer` on a fresh tree
# builds only what it names.
add_dependencies(installer xpcog-app xpcog-assets)
set_target_properties(installer PROPERTIES FOLDER packaging)
