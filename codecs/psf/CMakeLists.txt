# The PSF container, without a decoder.
#
# Not an xpcog_add_codec(): that macro exists to register a codec with the
# plugin registry, and there is nothing to register until an emulator core
# lands. Claiming `gsf` with no core behind it would put files in the open
# dialog that cannot play, which is the same mistake the module decoder's
# extension list is careful to avoid.
#
# So this is a plain library that the first core will link. It is worth landing
# on its own because every one of the eight cores needs exactly this and none of
# them needs it differently.
add_library(xpcog-codec-psf STATIC PsfFile.cpp)
target_include_directories(xpcog-codec-psf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(xpcog-codec-psf
    PUBLIC xpcog-core vendor-psflib xpcog-codec-common
    PRIVATE XPCog::warnings)
set_target_properties(xpcog-codec-psf PROPERTIES FOLDER codecs)
