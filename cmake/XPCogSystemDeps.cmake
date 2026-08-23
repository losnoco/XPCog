# Taking dependencies from the distribution instead of from vcpkg, on Linux.
#
# ---------------------------------------------------------------------------
# What this is for
# ---------------------------------------------------------------------------
# The Linux presets build 41 vcpkg packages, and a Linux machine with the -dev
# packages installed already has most of them: FFmpeg, curl, OpenSSL, libarchive
# and its five compressors, TagLib, SQLite, Opus, WavPack, libopenmpt, libgme,
# soxr, Catch2. Building those a second time buys nothing but wall clock -- the
# FFmpeg and OpenSSL ports alone are most of a cold build -- so where the system
# has one at a version this code can actually use, that copy is used and the port
# is never installed. On the machine this was written on that is 41 packages down
# to 14 -- 12 with the AUR's vgmstream-git installed as well, 11 with
# libspessasynth-git too -- and 643 MB of vcpkg_installed down to 46 MB.
#
# This is the same trade cmake/XPCogWx.cmake already makes for wxWidgets, and for
# the same reason. The difference is that wx is unconditional there (vcpkg's port
# drags in GTK, so it is never the right answer on Linux) while these are decided
# per machine, at configure time, against a version floor.
#
# Off by default and Linux-only. `-D XPCOG_USE_SYSTEM_LIBS=ON` turns it on, and
# the `linux-repo-debug` / `linux-repo-release` presets are that switch plus a
# build type. The plain `linux-*` presets keep taking everything from vcpkg, so
# CI and anyone chasing a reproducible build get a dependency set that does not
# vary with what happens to be installed.
#
# macOS and Windows are deliberately excluded. Homebrew's arm64/x86_64 split and
# the absence of a system package manager on Windows make "what is installed" a
# far less reliable question there, and neither platform has the Linux problem of
# rebuilding libraries the OS ships in its own SDK.
#
# ---------------------------------------------------------------------------
# Why the probe runs before project()
# ---------------------------------------------------------------------------
# vcpkg installs the manifest from inside its toolchain file, which CMake reads
# during project(). Everything that decides *what* to install therefore has to be
# settled before that line: by the time a find_package() could tell us the system
# has FFmpeg, vcpkg has already built its own. So the probe here is pkg-config
# run through execute_process(), which needs no compiler, no platform module and
# no toolchain -- just a program on PATH -- and its answers become
# VCPKG_MANIFEST_FEATURES.
#
# Which is also why every substitutable dependency is a *feature* in vcpkg.json
# rather than a plain entry under "dependencies": a plain dependency is always
# installed and there is no way to subtract one. They are listed under
# "default-features", so every other preset keeps getting them without saying
# anything, and this module turns VCPKG_MANIFEST_NO_DEFAULT_FEATURES on and names
# the survivors explicitly.
#
# ---------------------------------------------------------------------------
# What is not substitutable, and why
# ---------------------------------------------------------------------------
# `libogg`, `libflac`, `libvorbis`, `zlib`
#       codecs/flac and codecs/vorbis link three of them directly, and the
#       overlay ports build against all four: spessasynth-core for FLAC- and
#       Vorbis-compressed SF3 samples and deflated XMF nodes, libvgm and mgba for
#       zlib. Whichever way the ports go, vcpkg builds these four -- and linking
#       the system copy of a library that a vcpkg port was just compiled against
#       puts two of it in one process for no saving at all. They stay plain
#       dependencies.
#
# `mgba`, `libvgm`
#       No distribution packages them at the commit this builds against: mgba is
#       a kode54 fork, and libvgm is pinned to what Cog's plugins are written
#       for. mgba is the pointed case: a system libmgba is *available* on Arch
#       and Debian, and using it would be a mistake. `struct mCore` declares its
#       members inside #ifdef ENABLE_VFS / ENABLE_DIRECTORIES / MINIMAL_CORE, so
#       a library built with a different set has different member offsets --
#       which compiles, links, and then calls whatever happens to be in the slot.
#       See codecs/gsf/CMakeLists.txt for how that was found the first time.
#
# `libsidplayfp` and `vgmstream` are in the table but bounded above: see their
# entries. vgmstream and spessasynth-core are the two whose system copy no .pc
# file or config package describes, and whose distribution package tracks a
# repository rather than a release -- the first is held to the API version its
# header states, the second to the soname its library is installed under.

option(XPCOG_USE_SYSTEM_LIBS
       "Linux: take dependencies from the distribution when a new enough one is installed" OFF)

# Captured here rather than inside the function: CMAKE_CURRENT_LIST_DIR is the
# directory of the file that *defines* a function, which is this one, but saying
# so at module scope leaves nothing to check.
get_filename_component(XPCOG_VCPKG_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../vcpkg.json" REALPATH)

# ---------------------------------------------------------------------------
# The table
# ---------------------------------------------------------------------------
# One entry per dependency that a distribution can supply. Every version floor
# below is the oldest release carrying an API this code actually calls, and says
# which one -- a floor invented for tidiness would send a perfectly usable system
# library to vcpkg for nothing.
#
#   FEATURE   the vcpkg.json feature that installs the port, dropped from
#             VCPKG_MANIFEST_FEATURES when the system answers
#   MODULES   pkg-config specs, all of which must be satisfied
#   FILES     header + library names, for the libraries that ship no .pc file
#   MACROS    with FILES: #define names in that header, major/minor/patch, whose
#             values are the version the FILES probe has no .pc file to ask for
#   RANGE     with MACROS: the floor, and an exclusive ceiling where there is one
#   SOVERSION with FILES: the ABI the library must be installed under, for a
#             header tree that states no version of its own
#   TARGETS   imported targets the resolver creates, for the call sites whose
#             vcpkg path is a CONFIG-mode find_package that a system install
#             cannot satisfy. Omitted where the existing call site already finds
#             a system copy on its own -- see xpcog_system_dep() below.

set(XPCOG_SYSTEM_DEP_KEYS "")

macro(_xpcog_system_dep name label)
    cmake_parse_arguments(_D "" "FEATURE;SOVERSION"
                          "MODULES;FILES;MACROS;RANGE;TARGETS" ${ARGN})
    list(APPEND XPCOG_SYSTEM_DEP_KEYS "${name}")
    set(_xpcog_dep_${name}_LABEL   "${label}")
    set(_xpcog_dep_${name}_FEATURE "${_D_FEATURE}")
    set(_xpcog_dep_${name}_MODULES "${_D_MODULES}")
    set(_xpcog_dep_${name}_FILES   "${_D_FILES}")
    set(_xpcog_dep_${name}_MACROS  "${_D_MACROS}")
    set(_xpcog_dep_${name}_SOVERSION "${_D_SOVERSION}")
    set(_xpcog_dep_${name}_RANGE   "${_D_RANGE}")
    set(_xpcog_dep_${name}_TARGETS "${_D_TARGETS}")
endmacro()

# core/src/library/Sqlite.cpp: UPSERT (`ON CONFLICT ... DO UPDATE`), 3.24.0.
_xpcog_system_dep(sqlite3 "SQLite"
    FEATURE sqlite3
    MODULES "sqlite3 >= 3.24")

# core: soxr_quality_spec/soxr_io_spec/soxr_process, unchanged since 0.1.1.
# 0.1.2 is the floor because it is the oldest release still packaged anywhere.
_xpcog_system_dep(soxr "soxr"
    FEATURE soxr
    MODULES "soxr >= 0.1.2")

# codecs/taglib/TagLibReader.cpp includes <tvariant.h> and constructs a
# TagLib::FileName. TagLib::Variant is new in 2.0, and FileName became a class in
# the same release -- 1.13 has neither, which is what Ubuntu 24.04 ships.
_xpcog_system_dep(taglib "TagLib"
    FEATURE taglib
    MODULES "taglib >= 2.0"
    TARGETS TagLib::tag)

# libopenmpt's C++ interface, openmpt::module and get_supported_extensions().
# 0.6 is the oldest branch still receiving security fixes.
_xpcog_system_dep(openmpt "libopenmpt"
    FEATURE libopenmpt
    MODULES "libopenmpt >= 0.6"
    TARGETS libopenmpt::libopenmpt)

# codecs/gme calls gme_set_fade_msecs (0.6.1) and gme_load_custom.
_xpcog_system_dep(gme "libgme"
    FEATURE libgme
    MODULES "libgme >= 0.6.2"
    TARGETS GME::gme)

# codecs/archive: archive_read_open_filename_w and archive_entry_pathname_utf8.
_xpcog_system_dep(archive "libarchive"
    FEATURE libarchive
    MODULES "libarchive >= 3.4")

# codecs/httpsource: an easy handle and CURLOPT_XFERINFOFUNCTION-era options.
# 7.68 is Ubuntu 20.04's, and nothing here predates it.
_xpcog_system_dep(curl "libcurl"
    FEATURE curl
    MODULES "libcurl >= 7.68"
    TARGETS CURL::libcurl)

# codecs/opus: op_read_float, op_link_count, op_pcm_seek -- opusfile 0.9 and
# older is pre-release. opusfile pulls opus and ogg in through its own .pc.
_xpcog_system_dep(opus "opusfile"
    FEATURE opus
    MODULES "opusfile >= 0.10" "opus >= 1.3"
    TARGETS OpusFile::opusfile)

# codecs/wavpack: WavpackGetNumSamples64, WavpackSeekSample64 and
# WavpackGetNativeSampleRate are all WavPack 5.
_xpcog_system_dep(wavpack "WavPack"
    FEATURE wavpack
    MODULES "wavpack >= 5.0"
    TARGETS WavPack::WavPack)

# tests/: Catch2 v3, and catch_discover_tests() from the config package's own
# CMake module -- so this one is still found with find_package(Catch2 3 CONFIG),
# which reaches the system copy once vcpkg's is not installed.
#
# 3.7.1 rather than 3.3, where SKIP() itself arrived, and the gap is not
# pedantry. This suite skips a great deal by design -- every test that wants a
# corpus XPCOG_*_CORPUS did not name, which is most of the codec tests on a
# machine without the sample libraries -- and before 3.7.1 neither half of the
# machinery for reporting that existed:
#
#   All tests skipped is now always 4
#   catch_discover_tests now supports skipped tests (#2873)
#
# Below that, a binary whose every test skipped exits 2, ctest has no
# SKIP_RETURN_CODE registered for it, and each one is reported as a *failure*.
# Ubuntu 24.04's 3.4.0 does exactly that: 95 of 600 tests red, none of them
# broken. Found by running this configuration rather than by reading, which is
# what the Linux (distribution libraries) job in CI is for.
_xpcog_system_dep(catch2 "Catch2"
    FEATURE catch2
    MODULES "catch2-with-main >= 3.7.1")

# codecs/ffmpeg/FFmpegDecoder.cpp uses AVChannelLayout (codec_ctx_->ch_layout)
# and swr_alloc_set_opts2(), both new in FFmpeg 5.1. The library versions below
# are 5.1's. This is the entry that pays for the whole module: the vcpkg port is
# the longest build in the manifest, and it drags OpenSSL along with it.
_xpcog_system_dep(ffmpeg "FFmpeg, libavcodec"
    FEATURE ffmpeg
    MODULES "libavcodec >= 59.37.100" "libavformat >= 59.27.100"
            "libavutil >= 57.28.100" "libswresample >= 4.7.100")

# libsidplayfp, bounded above as well as below -- the only entry that is.
#
# sidplayfp 3.0 replaced `uint_least32_t play(short*, uint_least32_t)` with a
# cycle-driven `int play(unsigned int)` plus a separate mix step, and moved
# residfp.h from <sidplayfp/residfp.h> to <sidplayfp/builders/residfp.h>.
# codecs/sid/SidDecoder.cpp is written against the 2.x call and would not
# compile, so a 3.x system library is not a substitute for the pinned fork; the
# header move alone is handled with __has_include in the decoder.
_xpcog_system_dep(sid "libsidplayfp"
    FEATURE sid
    MODULES "libsidplayfp >= 2.3" "libsidplayfp < 3"
    TARGETS sidplayfp::sidplayfp)

# AdPlug, and this is the entry that settled how the three hand-assembled targets
# below are resolved.
#
# libbinio is not probed separately because adplug.pc says `Requires: libbinio >=
# 1.4`, so pkg-config answers for both or for neither. That is not the only thing
# it answers for. AdPlug's public headers include binio.h *unqualified*, and
# Debian installs it as /usr/include/libbinio/binio.h rather than beside adplug's
# own headers, which is a directory nothing could guess:
#
#   $ pkg-config --cflags --libs adplug
#   -I/usr/include/libbinio -ladplug -lbinio
#
# Which is why this entry, libgme's and libsidplayfp's all name a TARGETS, even
# though their call sites assemble an imported target from find_path() and
# find_library() and would have found *a* library without one. Those searches
# encode ports/adplug's layout, not a distribution's, and they carry no flags:
# Debian's libsidplayfp.pc puts -fopenmp in both Cflags and Libs. Asking
# pkg-config is asking the copy that is actually installed what it needs.
_xpcog_system_dep(adplug "AdPlug"
    FEATURE adplug
    MODULES "adplug >= 2.3.1"
    TARGETS AdPlug::adplug)

# libmpcdec ships no pkg-config file anywhere, so this one is probed by file.
# There is no version to check either: every distribution packages the same
# r4xx reference tree, and the four patches ports/libmpcdec carries are MSVC and
# undefined-behaviour fixes that a distribution build has already had to make.
#
# mpcenc is a separate question -- it is the encoder the Musepack test fixtures
# are built with, and tests/CMakeLists.txt looks for it on PATH when vcpkg has
# not built one.
_xpcog_system_dep(musepack "libmpcdec"
    FEATURE musepack
    FILES   "mpc/mpcdec.h" "mpcdec"
    TARGETS libmpcdec::mpcdec)

# vgmstream, and unlike every entry above it what a distribution packages is a
# *rolling* build: Arch's is `vgmstream-git`, and nobody packages a release
# tarball because upstream tags rNNNN and ships no other artifact. It installs
# neither a .pc file nor a CMake config package -- /usr/include/vgmstream/ and
# libvgmstream.so and nothing else -- so this is a FILES entry like libmpcdec's,
# held to the one version the install does state: LIBVGMSTREAM_API_VERSION_MAJOR
# and _MINOR, in libvgmstream.h.
#
# The floor is 1.0 because that is the whole of what this decoder uses:
# codecs/vgmstream/VgmStreamDecoder.cpp calls libvgmstream_create, _fill,
# _render, _seek, _free and _get_extensions and fills in a libstreamfile_t of its
# own. 1.1 added a libstreamfile_close helper it does not call.
#
# The ceiling is the reason the entry reads a version at all. That header calls
# MAJOR "breaking API/ABI changes", and the package tracking master means a 2.0
# arrives on whatever day upstream merges it -- with the existence check alone,
# on that day, the system copy would still be found, still be linked, and be
# compiled against a header describing different functions. ports/vgmstream is
# pinned to r2117 and cannot move underneath anyone; this can.
#
# What the two builds do not agree on is the optional codec set, and the
# direction it differs in is the harmless one. ports/vgmstream turns all of them
# off -- ATRAC9, CELT, Speex, G.719 and G.722.1 are *downloaded* at configure
# time, and MPEG and Vorbis need a .def-file fix on MSVC; the portfile says why
# in full -- while a distribution build has them on. So the system library
# decodes a superset, says so through libvgmstream_get_extensions(), and the
# registry picks the extensions up from there. Nothing in the API varies with
# which were compiled in, which is what separates this from mgba above.
_xpcog_system_dep(vgmstream "vgmstream"
    FEATURE vgmstream
    FILES   "vgmstream/libvgmstream.h" "vgmstream"
    MACROS  LIBVGMSTREAM_API_VERSION_MAJOR LIBVGMSTREAM_API_VERSION_MINOR
    RANGE   1.0 2.0
    TARGETS vgmstream::vgmstream)

# SpessaSynth Core, kode54's C port, behind codecs/midi's SoundFont synthesiser.
# Arch has it as the AUR's libspessasynth-git, which this repository's author also
# maintains. Like vgmstream it installs a header tree and a library and nothing
# else -- no .pc file, no config package -- so it is a FILES entry; unlike
# vgmstream it has no version macro anywhere in those headers, and unlike
# libmpcdec what is packaged is a rolling build of a fork that moves.
#
# What it does have is a soname, and upstream sets it by hand:
#
#     set_target_properties(spessasynth PROPERTIES VERSION 4.3.17 SOVERSION 10)
#
# which is the whole floor. Commit 28a362a ("Widen so many variables to double")
# changed member types float -> double across the *public* headers -- synth.h and
# the four DSP headers under synthesizer/dsp -- and ports/spessasynth-core is
# pinned past it. A distribution package still at soname 10 may be either side of
# that change and nothing in the install says which, since the break happened
# under an soname that did not move. So the floor is 11, the soname the break is
# given, and a machine whose package predates the bump keeps building the port.
#
# Note that this is a floor and not an equality even though an soname bump is
# normally an incompatibility: the fork is the one this project follows, and a
# later ABI is a later ABI of the same API, not a different library. If that
# stops being true it wants an upper bound here the way libsidplayfp has one.
#
# TARGETS, even though codecs/midi assembles spessasynth::spessasynth from
# find_path() and find_library() by exactly the names a distribution installs and
# would have landed on the system copy unaided. The reason is the same one
# codecs/gme has for putting its searches inside the fallback branch: find_path()
# and find_library() cache, under names this module does not own, and a
# reconfigure does not re-run them. Installing the package into a tree that was
# configured before it existed would otherwise leave both pointing into
# vcpkg_installed at a port that had just been uninstalled. Resolving here
# instead means the two branches are exclusive, and the cached variables of the
# branch not taken are never read.
#
# There is nothing else for TARGETS to carry, unlike codecs/adplug: no .pc file
# exists, so there are no flags to miss, and the FLAC, Vorbis and zlib the vcpkg
# branch has to name by hand are recorded in a shared library's own DT_NEEDED.
_xpcog_system_dep(spessasynth "SpessaSynth"
    FEATURE   spessasynth-core
    FILES     "spessasynth/spessasynth.h" "spessasynth"
    SOVERSION 11
    TARGETS   spessasynth::spessasynth)

# Reading a version out of a C header, for the FILES entries that have one to
# read. The MODULES entries get theirs from pkg-config; a library with no .pc
# file has to be asked some other way, and for a package that tracks upstream
# master -- vgmstream's, on every distribution that has one -- "the header is
# there" is not the same question as "the header is the one this code is written
# against".
#
# file(READ) and a regex, because that is all this can be: before project() there
# is no compiler to run a try_compile through, which is the usual way to ask a
# header what it says.
#
# Values are decimal or hex -- vgmstream writes 0x01 -- and math(EXPR) reads
# both, so every component goes through it on the way to a dotted version that
# VERSION_LESS can compare.
function(_xpcog_header_version header macros out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT EXISTS "${header}")
        return()
    endif()
    file(READ "${header}" _text)

    set(_parts "")
    foreach(_macro IN LISTS macros)
        # A macro that is missing means this is not the header the entry was
        # written against, so there is no version to report and the caller sends
        # the dependency to vcpkg -- the same answer as a floor not being met.
        if(NOT "${_text}" MATCHES "#[ \t]*define[ \t]+${_macro}[ \t]+(0[xX][0-9a-fA-F]+|[0-9]+)")
            return()
        endif()
        math(EXPR _value "${CMAKE_MATCH_1}")
        list(APPEND _parts "${_value}")
    endforeach()

    list(JOIN _parts "." _version)
    set(${out_var} "${_version}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The probe. Called from the top-level CMakeLists.txt *before* project().
# ---------------------------------------------------------------------------
function(xpcog_probe_system_deps)
    set(_report "")

    # Changing the answer inside a build tree that has already been configured is
    # refused rather than handled, and the reason is that most of the switching is
    # invisible from here. codecs/gme, codecs/sid and codecs/adplug find their
    # libraries with find_path() and find_library(), whose results are cached by
    # CMake itself under names this module does not own; core caches soxr's the
    # same way. Turning the option off would leave every one of those pointing at
    # /usr while vcpkg quietly rebuilt its copy -- a build that succeeds and is not
    # the one that was asked for.
    #
    # Each preset has its own binary directory, so this is only reachable by
    # editing the cache of an existing one by hand.
    if(DEFINED XPCOG_SYSTEM_LIBS_CONFIGURED AND
       NOT XPCOG_SYSTEM_LIBS_CONFIGURED STREQUAL "${XPCOG_USE_SYSTEM_LIBS}")
        message(FATAL_ERROR
                "XPCOG_USE_SYSTEM_LIBS changed from '${XPCOG_SYSTEM_LIBS_CONFIGURED}' "
                "to '${XPCOG_USE_SYSTEM_LIBS}' in a build directory that was already "
                "configured the other way.\n"
                "  Configure a fresh one instead -- the linux-debug and "
                "linux-repo-debug presets already build into separate directories, "
                "and that is why. Half of what this option moves is cached by "
                "find_library() under names this project does not control, so "
                "reusing the directory would keep some libraries from the side that "
                "is no longer in use.")
    endif()
    set(XPCOG_SYSTEM_LIBS_CONFIGURED "${XPCOG_USE_SYSTEM_LIBS}" CACHE INTERNAL
        "XPCOG_USE_SYSTEM_LIBS as of the configure that populated this directory")

    # CMAKE_HOST_SYSTEM_NAME is set from uname before any project() call, so this
    # is answerable here. CMAKE_SYSTEM_NAME is not -- it does not exist yet.
    if(XPCOG_USE_SYSTEM_LIBS AND NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        message(WARNING
                "XPCOG_USE_SYSTEM_LIBS is a Linux option and this is "
                "${CMAKE_HOST_SYSTEM_NAME}; ignoring it. See cmake/XPCogSystemDeps.cmake.")
    endif()

    if(NOT XPCOG_USE_SYSTEM_LIBS OR NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        foreach(_key IN LISTS XPCOG_SYSTEM_DEP_KEYS)
            set(XPCOG_SYSTEM_${_key} OFF CACHE INTERNAL "")
        endforeach()
        set(XPCOG_SYSTEM_DEPS_REPORT "" CACHE INTERNAL "")
        return()
    endif()

    # $PKG_CONFIG first: the same variable the FindPkgConfig module honours, so a
    # cross or hand-built pkgconf is respected here too rather than only later.
    if(DEFINED ENV{PKG_CONFIG})
        set(_pkgconfig "$ENV{PKG_CONFIG}")
    else()
        find_program(XPCOG_PKG_CONFIG NAMES pkg-config pkgconf)
        set(_pkgconfig "${XPCOG_PKG_CONFIG}")
    endif()
    if(NOT _pkgconfig)
        message(FATAL_ERROR
                "XPCOG_USE_SYSTEM_LIBS is on and pkg-config was not found.\n"
                "  Install pkg-config (or pkgconf), or configure without the "
                "option -- the plain linux-debug / linux-release presets take "
                "everything from vcpkg and need none of this.")
    endif()

    execute_process(COMMAND uname -m OUTPUT_VARIABLE _machine
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

    foreach(_key IN LISTS XPCOG_SYSTEM_DEP_KEYS)
        set(_found OFF)
        set(_version "")

        if(_xpcog_dep_${_key}_MODULES)
            # One --exists for the whole list: pkg-config takes several specs and
            # answers for the conjunction, which is what "all of these, at these
            # versions" means.
            execute_process(COMMAND "${_pkgconfig}" --exists ${_xpcog_dep_${_key}_MODULES}
                            RESULT_VARIABLE _rc ERROR_QUIET OUTPUT_QUIET)
            if(_rc EQUAL 0)
                set(_found ON)
                # The version reported is the first module's, which is the one the
                # entry is named for; the others are floors on its dependencies.
                list(GET _xpcog_dep_${_key}_MODULES 0 _first)
                string(REGEX REPLACE " *[<>=!].*$" "" _first "${_first}")
                execute_process(COMMAND "${_pkgconfig}" --modversion "${_first}"
                                OUTPUT_VARIABLE _version
                                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            endif()
        elseif(_xpcog_dep_${_key}_FILES)
            # No .pc file to ask. find_path()/find_library() are not usable here
            # -- before project() there is no platform module, so CMake does not
            # yet know what a library is called -- so the standard directories are
            # looked in directly. Deliberately only the standard ones: this is a
            # question about the distribution, and something unpacked elsewhere is
            # not what "installed" means.
            list(GET _xpcog_dep_${_key}_FILES 0 _header)
            list(GET _xpcog_dep_${_key}_FILES 1 _libname)
            set(_have_header OFF)
            set(_header_path "")
            foreach(_dir /usr/include /usr/local/include)
                if(EXISTS "${_dir}/${_header}")
                    set(_have_header ON)
                    set(_header_path "${_dir}/${_header}")
                endif()
            endforeach()
            set(_have_lib OFF)
            set(_abi_version "")
            set(_sover "${_xpcog_dep_${_key}_SOVERSION}")
            foreach(_dir /usr/lib "/usr/lib/${_machine}-linux-gnu" /usr/lib64
                         /usr/local/lib "/usr/local/lib/${_machine}-linux-gnu")
                if(_sover)
                    # An entry whose only version marker is the soname, so the
                    # question is not "is a library called this installed" but
                    # "is the one find_library() will pick the ABI we asked for".
                    # Those are different questions on a machine carrying two
                    # ABIs of the same library, so the link-time symlink and the
                    # required soname have to resolve to the *same file* rather
                    # than merely both exist.
                    set(_link "${_dir}/lib${_libname}.so")
                    set(_abi  "${_link}.${_sover}")
                    if(EXISTS "${_link}" AND EXISTS "${_abi}")
                        get_filename_component(_link_real "${_link}" REALPATH)
                        get_filename_component(_abi_real  "${_abi}"  REALPATH)
                        if(_link_real STREQUAL _abi_real)
                            set(_have_lib ON)
                            # Both point at the realname, which carries the
                            # release -- libspessasynth.so.4.3.17 -- and that is
                            # a more useful thing to print than the ABI alone.
                            set(_abi_version "soname ${_sover}")
                            get_filename_component(_real_name "${_abi_real}" NAME)
                            if(_real_name MATCHES "^lib${_libname}\\.so\\.([0-9][0-9.]*)$")
                                set(_abi_version "${CMAKE_MATCH_1}, soname ${_sover}")
                            endif()
                            break()
                        endif()
                    endif()
                else()
                    file(GLOB _hits "${_dir}/lib${_libname}.so" "${_dir}/lib${_libname}.a")
                    if(_hits)
                        set(_have_lib ON)
                    endif()
                endif()
            endforeach()
            if(_have_header AND _have_lib)
                set(_found ON)
                set(_version "installed")
                if(_abi_version)
                    set(_version "${_abi_version}")
                endif()
            endif()

            # A header this probe can read its own version out of, for an entry
            # that has one to read. file(READ) needs no more of CMake than
            # EXISTS above did, so this stays answerable before project() where
            # a try_compile would not be.
            if(_found AND _xpcog_dep_${_key}_MACROS)
                _xpcog_header_version("${_header_path}"
                                      "${_xpcog_dep_${_key}_MACROS}" _version)
                list(GET _xpcog_dep_${_key}_RANGE 0 _floor)
                list(LENGTH _xpcog_dep_${_key}_RANGE _range_len)
                if(NOT _version OR _version VERSION_LESS "${_floor}")
                    set(_found OFF)
                elseif(_range_len GREATER 1)
                    list(GET _xpcog_dep_${_key}_RANGE 1 _ceiling)
                    if(NOT _version VERSION_LESS "${_ceiling}")
                        set(_found OFF)
                    endif()
                endif()
            endif()
        endif()

        set(XPCOG_SYSTEM_${_key} ${_found} CACHE INTERNAL "system ${_key}")
        set(XPCOG_SYSTEM_${_key}_VERSION "${_version}" CACHE INTERNAL "")
        if(_found)
            list(APPEND _report "${_xpcog_dep_${_key}_LABEL}\t${_version}")
        endif()
    endforeach()

    # -----------------------------------------------------------------------
    # From answers to a manifest.
    #
    # Two lists come out of this. VCPKG_MANIFEST_FEATURES is what the preset
    # asked for minus everything the system satisfied, plus every default feature
    # it did not -- because the second half of the switch is
    # VCPKG_MANIFEST_NO_DEFAULT_FEATURES, without which vcpkg would install the
    # defaults again behind us.
    #
    # Both are set as ordinary variables rather than into the cache. The preset's
    # cache entry stays exactly as written, which keeps `cmake --preset` idempotent
    # and keeps the cache honest about what was asked for; the toolchain reads the
    # ordinary variable, which shadows it. Nothing else looks at either.
    # -----------------------------------------------------------------------
    # Which features are default features is read out of vcpkg.json rather than
    # repeated here, because the distinction decides whether an unsatisfied entry
    # is restated or left alone, and a copy of the list that drifted would be
    # wrong in a way nothing would catch. `ffmpeg`, `sid`, `adplug` and `musepack`
    # are in the table but are *not* defaults: a preset asks for those, and one
    # that does not ask must not be handed them here just because the system had
    # nothing to substitute.
    file(READ "${XPCOG_VCPKG_MANIFEST}" _manifest)
    string(JSON _default_count ERROR_VARIABLE _json_error LENGTH "${_manifest}" default-features)
    if(_json_error)
        message(FATAL_ERROR
                "XPCog: could not read \"default-features\" from ${XPCOG_VCPKG_MANIFEST}: "
                "${_json_error}\n"
                "  cmake/XPCogSystemDeps.cmake needs that list to know which vcpkg "
                "features are installed unless asked otherwise.")
    endif()
    set(_manifest_defaults "")
    math(EXPR _last "${_default_count} - 1")
    foreach(_i RANGE 0 ${_last})
        string(JSON _feature GET "${_manifest}" default-features ${_i})
        list(APPEND _manifest_defaults "${_feature}")
    endforeach()

    set(_defaults "")
    set(_requested "${VCPKG_MANIFEST_FEATURES}")
    set(_dropped "")

    foreach(_key IN LISTS XPCOG_SYSTEM_DEP_KEYS)
        set(_feature "${_xpcog_dep_${_key}_FEATURE}")
        if(XPCOG_SYSTEM_${_key})
            list(REMOVE_ITEM _requested "${_feature}")
            list(APPEND _dropped "${_feature}")
        elseif("${_feature}" IN_LIST _manifest_defaults)
            # A default feature the system did not answer for has to be named now
            # that the defaults are off. One that the preset already asked for is
            # already in _requested, and list(REMOVE_DUPLICATES) below sorts that
            # out rather than a membership test here.
            list(APPEND _defaults "${_feature}")
        endif()
    endforeach()

    # The features that are not in this table at all -- gui, sentry, psf-cores,
    # mgba, libvgm -- are untouched: they came from the preset and stay in
    # _requested. `sentry` is the one of those with a system package worth naming
    # and still not substitutable: what has to be staged beside the binary is the
    # crashpad handler from the same sentry-native this links against, and a
    # distribution's is a different build of a different crashpad. It also drags
    # vcpkg's curl in for its transport whatever this module decided about ours,
    # which is a real cost on a repo build and not one there is a way around.
    # spessasynth-core is in the table *and* a default feature, which is the
    # combination the restating branch above exists for: nothing asks for it by
    # name, so once the defaults are off it has to be named again unless the
    # system answered.
    # Only default features are re-stated, and only the unsatisfied ones, which
    # is the whole difference this module makes.
    set(_features ${_requested} ${_defaults})
    list(REMOVE_DUPLICATES _features)
    list(REMOVE_ITEM _features "")

    set(VCPKG_MANIFEST_FEATURES "${_features}" PARENT_SCOPE)
    set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON PARENT_SCOPE)
    set(XPCOG_SYSTEM_DEPS_REPORT "${_report}" CACHE INTERNAL "")


    list(JOIN _dropped " " _dropped_text)
    if(_dropped)
        message(STATUS "XPCog: system libraries in use, vcpkg features dropped: ${_dropped_text}")
    else()
        message(STATUS "XPCog: XPCOG_USE_SYSTEM_LIBS is on, but no system library "
                       "met its version floor -- every dependency still comes from vcpkg")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# The resolver. Called from a call site whose vcpkg path cannot find a system
# copy on its own, after project().
# ---------------------------------------------------------------------------
# Not every call site needs it. find_package(LibArchive), FindZLIB and
# FindSQLite3 search the system directories themselves and land on the system
# copy the moment vcpkg's is not installed, so those three are left alone.
#
# The rest are here for one of two reasons. CONFIG mode is the first: taglib,
# libopenmpt, WavPack, opusfile, curl, libmpcdec and vgmstream install no config
# package on a distribution, or none under the name vcpkg's port uses, so
# find_package(... CONFIG) cannot be the system path. The second is layout, and
# it is why codecs/gme, codecs/sid and codecs/adplug are here despite assembling
# their targets by hand from find_path() and find_library(): those searches would
# find a library and still be wrong, because they encode where the overlay port
# puts things and carry none of the flags the installed copy needs. See the
# adplug entry above for the case that proved it.
# So those call sites read:
#
#     xpcog_system_dep(taglib _system)
#     if(NOT _system)
#         find_package(taglib CONFIG REQUIRED GLOBAL)
#     endif()
#
# and this creates TagLib::tag from pkg-config so that everything downstream links
# the same target name either way.
function(xpcog_system_dep key out_var)
    set(${out_var} OFF PARENT_SCOPE)
    if(NOT XPCOG_SYSTEM_${key})
        return()
    endif()

    if(_xpcog_dep_${key}_MODULES)
        find_package(PkgConfig REQUIRED)
        # GLOBAL, for the reason cmake/XPCogCodec.cmake gives for passing GLOBAL to
        # find_package: an imported target made inside a codec's directory is
        # invisible to the xpcog-codecs aggregation that links it.
        pkg_check_modules(XPCOG_SYS_${key} REQUIRED IMPORTED_TARGET GLOBAL
                          ${_xpcog_dep_${key}_MODULES})
        foreach(_target IN LISTS _xpcog_dep_${key}_TARGETS)
            if(NOT TARGET ${_target})
                add_library(${_target} INTERFACE IMPORTED GLOBAL)
                set_property(TARGET ${_target} PROPERTY
                             INTERFACE_LINK_LIBRARIES PkgConfig::XPCOG_SYS_${key})
            endif()
        endforeach()
        # FFmpeg names no target: its call sites consume the same
        # FFMPEG_LIBRARIES / FFMPEG_INCLUDE_DIRS pair that vcpkg's FindFFMPEG.cmake
        # sets, so the branch is invisible to codecs/ffmpeg and tests/.
        if("${key}" STREQUAL "ffmpeg")
            set(FFMPEG_LIBRARIES    PkgConfig::XPCOG_SYS_ffmpeg PARENT_SCOPE)
            set(FFMPEG_INCLUDE_DIRS "${XPCOG_SYS_ffmpeg_INCLUDE_DIRS}" PARENT_SCOPE)
        endif()
    elseif(_xpcog_dep_${key}_FILES)
        list(GET _xpcog_dep_${key}_FILES 0 _header)
        list(GET _xpcog_dep_${key}_FILES 1 _libname)
        # REQUIRED: the pre-project probe already saw both of these, so failing
        # here means they moved between then and now, which is worth an error
        # rather than a quiet fallback to a port that is no longer installed.
        find_path(XPCOG_SYS_${key}_INCLUDE_DIR NAMES "${_header}" REQUIRED)
        find_library(XPCOG_SYS_${key}_LIBRARY NAMES "${_libname}" REQUIRED)
        foreach(_target IN LISTS _xpcog_dep_${key}_TARGETS)
            if(NOT TARGET ${_target})
                add_library(${_target} UNKNOWN IMPORTED GLOBAL)
                set_target_properties(${_target} PROPERTIES
                    IMPORTED_LOCATION "${XPCOG_SYS_${key}_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${XPCOG_SYS_${key}_INCLUDE_DIR}")
            endif()
        endforeach()
    endif()

    set(${out_var} ON PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Imported configurations. Called from the top-level CMakeLists.txt immediately
# *after* project(), and both halves of that matter.
# ---------------------------------------------------------------------------
# A config package built by vcpkg carries a Debug configuration and a Release
# configuration and nothing else, so a RelWithDebInfo build of this project
# already depends on being sent from RelWithDebInfo to Release -- and vcpkg's
# toolchain arranges exactly that, by setting
# CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO to "RelWithDebInfo;Release;" as it is
# read, which is during project().
#
# A distribution's package is built once, with no configuration at all. Arch's
# Catch2 exports its library under `NOCONFIG`, which is not in vcpkg's map -- and
# a map that exists and matches nothing is final, because naming one turns off the
# fallback to whatever the target does have. So `Catch2::Catch2WithMain` reaches
# the generate step with no IMPORTED_LOCATION for the configuration being built
# and CMake stops there:
#
#   CMake Error in tests/CMakeLists.txt:
#     IMPORTED_LOCATION not set for imported target "Catch2::Catch2WithMain"
#     configuration "RelWithDebInfo".
#
# Adding the missing entries fixes it in every direction at once. Each list still
# starts with the configuration actually asked for, so a package that has it --
# every vcpkg one -- never reaches the rest; an explicit Debug build of a
# dependency is last, since a release library is the better answer for a
# RelWithDebInfo build than a debug one. The trailing empty entry is CMake's
# spelling for the un-suffixed IMPORTED_LOCATION.
#
# After project(), because vcpkg's toolchain writes these variables itself and
# would overwrite anything set before that line.
function(xpcog_system_deps_config_map)
    if(NOT XPCOG_SYSTEM_DEPS_REPORT)
        return()
    endif()
    set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG
        "Debug;None;NOCONFIG;RelWithDebInfo;Release;" PARENT_SCOPE)
    set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO
        "RelWithDebInfo;Release;None;NOCONFIG;Debug;" PARENT_SCOPE)
    set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE
        "Release;RelWithDebInfo;None;NOCONFIG;Debug;" PARENT_SCOPE)
    set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL
        "MinSizeRel;Release;RelWithDebInfo;None;NOCONFIG;Debug;" PARENT_SCOPE)
endfunction()

# The configure summary's share of this, printed from the top-level CMakeLists.
function(xpcog_system_deps_report)
    if(NOT XPCOG_SYSTEM_DEPS_REPORT)
        return()
    endif()
    message(STATUS "  system libraries ... (vcpkg not used for these)")
    foreach(_line IN LISTS XPCOG_SYSTEM_DEPS_REPORT)
        string(REPLACE "\t" " " _line "${_line}")
        message(STATUS "    ${_line}")
    endforeach()
endfunction()
