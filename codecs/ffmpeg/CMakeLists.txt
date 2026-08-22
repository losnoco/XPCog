# FindFFMPEG.cmake is vcpkg's own, installed by its ffmpeg port, so there is
# nothing to find in MODULE mode without it. The system branch sets the same two
# variables from the four libav* .pc files; nothing below can tell which ran.
xpcog_system_dep(ffmpeg _ffmpeg_from_system)
if(NOT _ffmpeg_from_system)
    find_package(FFMPEG REQUIRED)
endif()

xpcog_add_codec(
    NAME     ffmpeg
    REGISTER xpcog_register_ffmpeg
    SOURCES  FFmpegDecoder.cpp
    DEPS     ${FFMPEG_LIBRARIES} xpcog-codec-common
    INCLUDES ${FFMPEG_INCLUDE_DIRS}
)
