# ---------------------------------------------------------------------------
# The oldest macOS XPCog runs on, in the one place both halves of the build read.
#
# There are two builds on macOS, not one. The root CMakeLists configures XPCog
# itself; vcpkg configures every dependency separately, driven by the triplet,
# in a CMake process that inherits nothing from ours. Each has its own name for
# the same number -- CMAKE_OSX_DEPLOYMENT_TARGET here, VCPKG_OSX_DEPLOYMENT_TARGET
# in the triplet -- and neither reads the other's.
#
# Left to disagree they disagree silently. That is what shipped in 1.5.1: the
# executable was stamped minos 13.0 and every dependency under it was built by a
# macos-26 runner with no deployment target set at all, which makes clang target
# the host. Contents/Frameworks/libvgmstream.dylib came out minos 26.0, and
# because it is the executable's one @rpath dependency dyld resolves it before
# main runs and refuses to load it. The app died at launch on macOS 13 while
# every build and test on the runner passed.
#
# So the number lives here, and this file is included twice: by the root
# CMakeLists before project(), and by cmake/XPCogOsxTriplet.cmake, which the
# overlay triplets in triplets/ include. Nothing else may spell it out.
#
# 13.0 is Ventura. Raising it is a user-visible compatibility change; see the
# semver rules in CLAUDE.md before doing it.
# ---------------------------------------------------------------------------
set(XPCOG_OSX_DEPLOYMENT_TARGET "13.0")
