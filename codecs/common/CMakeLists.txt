# Shared codec helpers. Not a codec itself, so no xpcog_add_codec() and no
# registrar -- codecs link this for tag handling they would otherwise duplicate.
add_library(xpcog-codec-common STATIC VorbisComments.cpp PlaylistText.cpp TextEncoding.cpp SourceBytes.cpp)
target_include_directories(xpcog-codec-common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(xpcog-codec-common PUBLIC xpcog-core PRIVATE XPCog::warnings)
set_target_properties(xpcog-codec-common PROPERTIES FOLDER codecs)
