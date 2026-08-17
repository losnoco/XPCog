find_package(FFMPEG REQUIRED)

xpcog_add_codec(
    NAME     ffmpeg
    REGISTER xpcog_register_ffmpeg
    SOURCES  FFmpegDecoder.cpp
    DEPS     ${FFMPEG_LIBRARIES}
    INCLUDES ${FFMPEG_INCLUDE_DIRS}
)
