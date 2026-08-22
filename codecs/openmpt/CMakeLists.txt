# The system branch, when XPCOG_USE_SYSTEM_LIBS found one: distributions install
# libopenmpt.pc but no CMake config package, so the target is made from that.
xpcog_system_dep(openmpt _openmpt_from_system)
if(NOT _openmpt_from_system)
    find_package(libopenmpt CONFIG REQUIRED GLOBAL)
endif()

# The compressed module extensions -- itz, mdz and the rest -- are one module in
# a zip, and it is the archive codec that unwraps them. Without it this decoder
# must not claim them, or the open dialog offers files that cannot be played.
set(_openmpt_defines "")
if(XPCOG_WITH_ARCHIVE)
    list(APPEND _openmpt_defines XPCOG_HAVE_ARCHIVE=1)
endif()

xpcog_add_codec(
    NAME     openmpt
    REGISTER xpcog_register_openmpt
    SOURCES  OpenMptDecoder.cpp
    DEPS     libopenmpt::libopenmpt xpcog-codec-common
    DEFINES  ${_openmpt_defines}
)
