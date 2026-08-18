# FreeSurround's channel allocation tables, from Cog Audio/ThirdParty/fsurround
# (Christian Kothe, GPL v2 or later).
#
# The data only. channelmaps.cpp is ~3,100 lines of literal grids -- one 21x21
# gain surface per output channel per setup -- and is vendored verbatim, because
# it has no Apple dependency, no arithmetic worth reviewing, and every character
# of it is a number that has to survive the port exactly.
#
# The decoder that used to sit beside it is NOT here. It is rewritten in
# core/src/audio/FreeSurround.cpp against XPCog's own FFT, so it depends on core
# and cannot live in a library core links. Splitting them along that line is not
# a compromise: it is the line between what was copied and what was rewritten.
#
# See README.md for what was changed and what was not.
add_library(vendor-fsurround STATIC channelmaps.cpp)
target_include_directories(vendor-fsurround PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(vendor-fsurround PROPERTIES FOLDER vendor)
if(NOT MSVC)
    target_compile_options(vendor-fsurround PRIVATE -w)
endif()
