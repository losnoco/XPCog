# Codec registration.
#
# Why generated registration rather than self-registering statics:
# a `static Registrar<Foo> reg_;` in a codec .cpp does NOT work inside a static
# library. The linker only extracts an archive member that resolves an undefined
# symbol; nothing references reg_, so the object is dropped and the codec silently
# vanishes at runtime. The usual escapes (--whole-archive / -force_load /
# /WHOLEARCHIVE:) are per-linker, bloat the binary, and fight -dead_strip and LTO.
#
# Instead each codec exports exactly one registrar function, and codecs/CMakeLists.txt
# generates a RegisterAll.cpp that declares and calls every one of them in order.
# Deterministic, greppable, breakpointable, and it scales the same at 8 codecs as at 35.
#
# Codecs that resolve a library with find_package() must pass GLOBAL, so the
# imported target escapes the codec's directory scope and is visible to the
# xpcog-codecs aggregation:  find_package(FLAC CONFIG REQUIRED GLOBAL)
#
#   xpcog_add_codec(
#       NAME     flac
#       REGISTER xpcog_register_flac
#       SOURCES  FlacDecoder.cpp Register.cpp
#       DEPS     FLAC::FLAC
#       DEFINES  SOMETHING=1)

function(xpcog_add_codec)
    cmake_parse_arguments(C "" "NAME;REGISTER" "SOURCES;DEPS;DEFINES;INCLUDES" ${ARGN})

    if(NOT C_NAME)
        message(FATAL_ERROR "xpcog_add_codec: NAME is required")
    endif()
    if(NOT C_REGISTER)
        message(FATAL_ERROR "xpcog_add_codec(${C_NAME}): REGISTER is required")
    endif()
    if(NOT C_SOURCES)
        message(FATAL_ERROR "xpcog_add_codec(${C_NAME}): SOURCES is required")
    endif()

    set(_target xpcog-codec-${C_NAME})
    add_library(${_target} OBJECT ${C_SOURCES})
    target_link_libraries(${_target} PUBLIC xpcog-core ${C_DEPS})
    target_link_libraries(${_target} PRIVATE XPCog::warnings)
    if(C_DEFINES)
        target_compile_definitions(${_target} PRIVATE ${C_DEFINES})
    endif()
    if(C_INCLUDES)
        target_include_directories(${_target} PRIVATE ${C_INCLUDES})
    endif()
    set_target_properties(${_target} PROPERTIES FOLDER codecs)

    set_property(GLOBAL APPEND PROPERTY XPCOG_CODEC_TARGETS   ${_target})
    set_property(GLOBAL APPEND PROPERTY XPCOG_CODEC_REGISTRARS ${C_REGISTER})
    # Tracked separately rather than read back from INTERFACE_LINK_LIBRARIES,
    # which would also carry the $<LINK_ONLY:> wrapper around the private
    # warnings interface and fail to resolve in the aggregating target.
    if(C_DEPS)
        set_property(GLOBAL APPEND PROPERTY XPCOG_CODEC_DEPS ${C_DEPS})
    endif()
endfunction()
