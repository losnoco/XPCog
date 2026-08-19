# GLOBAL, or the imported target stays inside this directory scope and the
# xpcog-codecs aggregation cannot see it. See cmake/XPCogCodec.cmake.
find_package(LibArchive REQUIRED GLOBAL)

# The libarchive wrapper on its own, because it now has a consumer outside this
# directory: codecs/midi reads a Roland ROM set straight out of the zip it was
# distributed in, and that is the same handle lifetime and the same read loop.
# A static library rather than a fourth source in the codec below, for the
# reason codecs/psf is one -- an OBJECT library's objects would arrive twice in
# xpcog-codecs, once through each consumer.
add_library(xpcog-archive-reader STATIC ArchiveReader.cpp)
target_include_directories(xpcog-archive-reader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(xpcog-archive-reader
    PUBLIC xpcog-core LibArchive::LibArchive
    PRIVATE xpcog-codec-common XPCog::warnings)
set_target_properties(xpcog-archive-reader PROPERTIES FOLDER codecs)

# The codec itself is optional in a way the reader is not: a build with MIDI but
# without archive support still wants the reader, and still has no business
# offering a .zip as a playlist.
if(XPCOG_WITH_ARCHIVE)
    xpcog_add_codec(
        NAME     archive
        REGISTER xpcog_register_archive
        SOURCES  ArchiveSource.cpp CompressedFileSource.cpp UnpackUrl.cpp
        DEPS     xpcog-archive-reader xpcog-codec-common
    )
endif()
