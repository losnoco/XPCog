# Finding Qt.
#
# Qt deliberately does not come from vcpkg -- mixing a vcpkg Qt with vcpkg's
# other ports is a well-known source of grief -- so it has to be located in
# whatever installation the developer already has. That location differs on
# every machine and every platform, which makes it exactly the wrong thing to
# hard-code in a committed preset.
#
# The answer is one environment variable. Set XPCOG_QT_ROOT to the directory
# holding `bin` and `lib/cmake` -- the one aqtinstall and the Qt online
# installer produce:
#
#   macOS    ~/Qt/6.11.1/macos
#   Linux    ~/Qt/6.11.1/gcc_64
#   Windows  C:/Qt/6.11.1/msvc2022_64
#
# It may also be given as a CMake cache variable, which takes precedence, for
# the case where several Qt versions are being compared. Anyone who prefers to
# set CMAKE_PREFIX_PATH themselves can still do that and ignore this entirely.

if(NOT XPCOG_BUILD_APP)
    return()
endif()

if(NOT XPCOG_QT_ROOT AND DEFINED ENV{XPCOG_QT_ROOT})
    set(XPCOG_QT_ROOT "$ENV{XPCOG_QT_ROOT}")
endif()

if(XPCOG_QT_ROOT)
    file(TO_CMAKE_PATH "${XPCOG_QT_ROOT}" XPCOG_QT_ROOT)
    if(NOT EXISTS "${XPCOG_QT_ROOT}/lib/cmake/Qt6")
        # Better here than as "could not find Qt6Config.cmake" fifty lines
        # later, which sends people looking for a missing package rather than a
        # mistyped path.
        message(FATAL_ERROR
            "XPCOG_QT_ROOT is set to '${XPCOG_QT_ROOT}', but there is no "
            "lib/cmake/Qt6 under it. It should point at a Qt installation "
            "directory such as ~/Qt/6.11.1/macos or C:/Qt/6.11.1/msvc2022_64.")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${XPCOG_QT_ROOT}")
    message(STATUS "  Qt ................. ${XPCOG_QT_ROOT} (XPCOG_QT_ROOT)")
endif()
