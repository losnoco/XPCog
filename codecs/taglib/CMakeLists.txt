# GLOBAL promotes the imported target out of this directory scope so the
# xpcog-codecs aggregation in ../CMakeLists.txt can link it.
find_package(taglib CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     taglib
    REGISTER xpcog_register_taglib
    SOURCES  TagLibReader.cpp
    DEPS     TagLib::tag
)
