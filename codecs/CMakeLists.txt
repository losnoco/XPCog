# Each codec calls xpcog_add_codec() (see cmake/XPCogCodec.cmake), which appends to
# two global properties. The tail of this file turns those into a generated
# RegisterAll.cpp with an explicit call to every registrar.

add_subdirectory(common)
add_subdirectory(filesource)

if(XPCOG_WITH_FLAC)
    add_subdirectory(flac)
endif()
if(XPCOG_WITH_VORBIS)
    add_subdirectory(vorbis)
endif()
if(XPCOG_WITH_OPUS)
    add_subdirectory(opus)
endif()
if(XPCOG_WITH_MPG123)
    add_subdirectory(mpg123)
endif()
if(XPCOG_WITH_WAVPACK)
    add_subdirectory(wavpack)
endif()
if(XPCOG_WITH_PLAYLISTS)
    add_subdirectory(playlists)
endif()
if(XPCOG_WITH_FFMPEG)
    add_subdirectory(ffmpeg)
endif()
if(XPCOG_WITH_CUESHEET)
    add_subdirectory(cuesheet)
endif()
if(XPCOG_WITH_TAGLIB)
    add_subdirectory(taglib)
endif()
if(XPCOG_WITH_HTTP)
    add_subdirectory(httpsource)
endif()
if(XPCOG_WITH_OPENMPT)
    add_subdirectory(openmpt)
endif()

# M1b onward: ffmpeg, vorbis, opus, mpg123, wavpack, ape, musepack, taglib,
# cuesheet, m3u, pls, xmlplaylist. M6: the remaining ~27 Cog decoders.

# ---------------------------------------------------------------------------
# Generate RegisterAll.cpp
# ---------------------------------------------------------------------------
get_property(_registrars GLOBAL PROPERTY XPCOG_CODEC_REGISTRARS)
get_property(_codec_targets GLOBAL PROPERTY XPCOG_CODEC_TARGETS)

set(XPCOG_REG_DECLS "")
set(XPCOG_REG_CALLS "")
foreach(_r IN LISTS _registrars)
    string(APPEND XPCOG_REG_DECLS "namespace xpcog { class PluginRegistry; }\nvoid ${_r}(::xpcog::PluginRegistry&);\n")
    string(APPEND XPCOG_REG_CALLS "    ${_r}(r);\n")
endforeach()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/RegisterAll.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/RegisterAll.cpp"
    @ONLY)

add_library(xpcog-codecs STATIC "${CMAKE_CURRENT_BINARY_DIR}/RegisterAll.cpp")
add_library(XPCog::codecs ALIAS xpcog-codecs)

target_link_libraries(xpcog-codecs PUBLIC xpcog-core)
target_link_libraries(xpcog-codecs PRIVATE XPCog::warnings)

# OBJECT libraries contribute their objects directly, so nothing can be dropped by
# the archive-member selection rule that breaks self-registration.
foreach(_t IN LISTS _codec_targets)
    target_sources(xpcog-codecs PRIVATE $<TARGET_OBJECTS:${_t}>)
endforeach()

# Codec libraries must reach the final link, so they propagate PUBLIC.
get_property(_codec_deps GLOBAL PROPERTY XPCOG_CODEC_DEPS)
if(_codec_deps)
    list(REMOVE_DUPLICATES _codec_deps)
    target_link_libraries(xpcog-codecs PUBLIC ${_codec_deps})
endif()

set_target_properties(xpcog-codecs PROPERTIES FOLDER codecs)
