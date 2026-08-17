# miniaudio 0.11.25 -- https://github.com/mackron/miniaudio (public domain / MIT-0)
#
# Single-header library. The implementation is compiled exactly once, in
# miniaudio.c, so every other translation unit gets only the declarations.

add_library(vendor-miniaudio STATIC miniaudio.c)

target_include_directories(vendor-miniaudio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Trim backends XPCog does not use. Decoding, encoding and the resampler come
# from dedicated libraries (libFLAC, soxr), never from miniaudio.
target_compile_definitions(vendor-miniaudio PUBLIC
    MA_NO_DECODING
    MA_NO_ENCODING
    MA_NO_GENERATION
    MA_NO_RESOURCE_MANAGER
    MA_NO_NODE_GRAPH)

if(APPLE)
    find_library(COREAUDIO_LIBRARY CoreAudio REQUIRED)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox REQUIRED)
    target_link_libraries(vendor-miniaudio PUBLIC
        ${COREAUDIO_LIBRARY} ${COREFOUNDATION_LIBRARY} ${AUDIOTOOLBOX_LIBRARY})
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(vendor-miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    target_link_libraries(vendor-miniaudio PUBLIC m)
endif()

set_target_properties(vendor-miniaudio PROPERTIES FOLDER vendor)

# Vendored code is not held to the project's warning settings.
if(NOT MSVC)
    target_compile_options(vendor-miniaudio PRIVATE -w)
endif()
