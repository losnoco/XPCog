# Musepack, on libmpcdec. An overlay port rather than a vendored copy; see
# ../../ports/libmpcdec/portfile.cmake for what it patches and why.
find_package(libmpcdec CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     musepack
    REGISTER xpcog_register_musepack
    SOURCES  MusepackDecoder.cpp
    DEPS     libmpcdec::mpcdec
)
