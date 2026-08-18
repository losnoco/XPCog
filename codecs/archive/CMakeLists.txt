# GLOBAL, or the imported target stays inside this directory scope and the
# xpcog-codecs aggregation cannot see it. See cmake/XPCogCodec.cmake.
find_package(LibArchive REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     archive
    REGISTER xpcog_register_archive
    SOURCES  ArchiveSource.cpp ArchiveReader.cpp CompressedFileSource.cpp UnpackUrl.cpp
    DEPS     LibArchive::LibArchive xpcog-codec-common
)
