# GLOBAL promotes the imported target out of this directory scope so the
# xpcog-codecs aggregation in ../CMakeLists.txt can link it. Every codec that
# pulls in a library via find_package needs this.
find_package(FLAC CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     flac
    REGISTER xpcog_register_flac
    SOURCES  FlacDecoder.cpp Register.cpp
    DEPS     FLAC::FLAC xpcog-codec-common
)
