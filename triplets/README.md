# vcpkg overlay triplets

Two files, `arm64-osx` and `x64-osx`, each vcpkg's own triplet of that name plus
an include of [`../cmake/XPCogOsxTriplet.cmake`](../cmake/XPCogOsxTriplet.cmake).
`vcpkg-configuration.json` registers this directory as `overlay-triplets`, so
they take precedence over the built-in and community ones without anyone naming
a triplet on the command line.

There is no Windows or Linux triplet here, and there should not be one until
something needs it. Neither platform has the problem this directory exists for.

## What the include changes, and why a triplet is where it has to go

**The deployment target.** `CMAKE_OSX_DEPLOYMENT_TARGET` governs XPCog's own
build tree and nothing else. vcpkg configures each dependency in a separate
CMake process driven by the triplet, and inherits no value from the project
consuming it — `scripts/toolchains/osx.cmake` does not mention a deployment
target at all, and the only thing that reads `VCPKG_OSX_DEPLOYMENT_TARGET` is
`vcpkg_cmake_configure()`, which forwards it if the triplet defined it. The
built-in `arm64-osx` does not, so clang fell back to targeting the build
machine.

That is what shipped in 1.5.1. Built on a `macos-26` runner:

| Mach-O in `XPCog.app` | `minos` |
| --- | --- |
| `Contents/MacOS/XPCog` | 13.0 |
| `Contents/Frameworks/libvgmstream.dylib` | 26.0 |
| `Contents/MacOS/crashpad_handler` | 26.0 |

`libvgmstream.dylib` is the executable's one `@rpath` dependency, so dyld
resolves it before `main` and refuses a library built for a newer OS than the
one running. The app died at launch on macOS 13 with every build, test and
notarisation step green. The static libraries were the same — `libavcodec.a` and
`libFLAC.a` built `minos 27.0` on a developer machine — which is worse in a
quieter way: they link *into* an executable that claims 13.0, having compiled
with every availability guard above 13 switched off.

The number itself is in
[`../cmake/XPCogOsxDeploymentTarget.cmake`](../cmake/XPCogOsxDeploymentTarget.cmake),
which the root `CMakeLists.txt` also includes, before `project()`. One file, two
readers; the two builds cannot drift.

**pkg-config's search path.** vcpkg leaves the system `.pc` path reachable
deliberately, and sets `PKG_CONFIG_PATH` only from `vcpkg_configure_make()`. A
port that builds with CMake and calls `pkg_check_modules()` therefore sees
Homebrew's `/opt/homebrew/lib/pkgconfig` and nothing of vcpkg's. wxWidgets
resolves PCRE2 exactly that way, so on a Mac with Homebrew's pcre2 installed the
exported `wxWidgetsTargets.cmake` named
`/opt/homebrew/Cellar/pcre2/10.47_1/lib/libpcre2-32.dylib`, and XPCog linked a
versioned Cellar path that exists on one machine.

`PKG_CONFIG_PATH` is the lever, not `PKG_CONFIG_LIBDIR`. `LIBDIR` is the one
that reads right — it is documented to replace the compiled-in search path
rather than extend it — but Homebrew's pkgconf ignores it:

```sh
PKG_CONFIG_LIBDIR=/nonexistent pkg-config --exists libpcre2-32   # still succeeds
```

So being *earlier* in the search order is all that is available, which is what
`PKG_CONFIG_PATH` gives. Homebrew stays reachable for anything vcpkg does not
build; where vcpkg has its own copy, its own copy wins.

## Changing either of them

Both are inputs to vcpkg's ABI hash, so editing these files or the two under
`cmake/` invalidates every cached macOS binary and rebuilds the world. That is
correct — the old ones are mis-targeted — but it is not a five-minute change.
`.github/workflows/ci.yml` hashes `triplets/**` and the two `cmake/` files into
the vcpkg cache key so the runner's cache rotates with them.
