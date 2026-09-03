# ---------------------------------------------------------------------------
# Everything the macOS triplets have in common, which is everything except the
# architecture. Included by triplets/arm64-osx.cmake and triplets/x64-osx.cmake;
# it lives here rather than in triplets/ because vcpkg reads every .cmake file
# in an overlay triplet directory as a triplet named after that file, and a
# fragment sitting in there would look like one.
#
# Read as part of vcpkg's ports.cmake, which defines CURRENT_INSTALLED_DIR
# before it includes the triplet. Ordinary XPCog CMake code must not include
# this file -- it has environment side effects that only make sense inside a
# port build. XPCogOsxDeploymentTarget.cmake is the part that is safe to share,
# and is the only part the root CMakeLists takes.
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/XPCogOsxDeploymentTarget.cmake")

# vcpkg_cmake_configure() forwards this as -DCMAKE_OSX_DEPLOYMENT_TARGET, and
# scripts/get_cmake_vars folds it into the detected compiler flags, which is how
# the autotools and meson ports -- FFmpeg above all -- get it too. Undefined,
# which is what the built-in arm64-osx triplet leaves it, clang targets whatever
# the build machine happens to be running. See XPCogOsxDeploymentTarget.cmake
# for what that shipped.
set(VCPKG_OSX_DEPLOYMENT_TARGET "${XPCOG_OSX_DEPLOYMENT_TARGET}")

# Put vcpkg's .pc files ahead of Homebrew's.
#
# pkg-config's compiled-in search path starts at /opt/homebrew/lib/pkgconfig,
# and vcpkg does not displace it: z_vcpkg_setup_pkgconfig_path() only prepends
# to PKG_CONFIG_PATH, leaving the system path reachable on purpose as a last
# resort, and it is called by vcpkg_configure_make() alone. A port that builds
# with CMake and calls pkg_check_modules() gets no vcpkg paths at all, so the
# first .pc file it can see is Homebrew's.
#
# wxWidgets is one of those. Its FindPCRE2.cmake resolves PCRE2 through
# pkg_check_modules, so on any Mac with Homebrew's pcre2 installed the wx build
# found it, and vcpkg's exported wxWidgetsTargets.cmake came out naming
# /opt/homebrew/Cellar/pcre2/<version>/lib/libpcre2-32.dylib. XPCog then linked
# a Cellar path -- versioned, and absent on every machine but the one that
# built it -- into a bundle that has no business loading anything outside
# itself. CI happened not to have pcre2 in Homebrew, so 1.5.1 escaped it; that
# is luck, not a property of the build.
#
# PKG_CONFIG_PATH and not PKG_CONFIG_LIBDIR, which is the one that sounds right:
# LIBDIR is meant to *replace* the compiled-in path rather than extend it, but
# Homebrew's pkgconf ignores it -- `PKG_CONFIG_LIBDIR=/nonexistent pkg-config
# --exists libpcre2-32` still succeeds on a Mac with Homebrew's pcre2. So the
# only lever that works here is to be earlier in the search order, which is what
# PKG_CONFIG_PATH is. Homebrew stays reachable for anything vcpkg does not
# build; where vcpkg has its own copy, its own copy wins.
#
# Both consumers preserve this. CMake's FindPkgConfig saves PKG_CONFIG_PATH and
# appends to it, so ours stays in front; vcpkg_configure_make() prepends its
# per-configuration dirs, so a debug port still resolves debug .pc files first
# and these are its fallback. A CMake port has no such hook and will read the
# release .pc in both configurations -- a static C library either way on macOS,
# and a great deal better than reading Homebrew's.
#
# The literal ":" is correct here and nowhere else: these triplets are macOS.
set(ENV{PKG_CONFIG_PATH} "${CURRENT_INSTALLED_DIR}/lib/pkgconfig:${CURRENT_INSTALLED_DIR}/share/pkgconfig")
