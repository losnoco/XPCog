# Organya has no library to find: the whole player is OrganyaSynth.cpp, which is
# how Cog carries it too. What it does need is data.
#
# `wavetable.dat` is the bank of a hundred melodic waveforms, and the six
# `fx9*.pxt` files are the PixTone parameter sets the percussion is synthesised
# from. Both are Pixel's, extracted from Cave Story and shipped by every
# Organya player there has ever been -- Cog puts them in its plugin bundle and
# looks them up through NSBundle at startup.
#
# There is no bundle here and there are three platforms, so they are compiled in:
# 34 KB total, turned into byte arrays by CMake at build time. That removes an
# install step, a search path, and the failure mode where the data is missing on
# somebody's machine and the symptom is a song with no drums. It is the same
# arrangement, and for the same reasons, as the AdPlug song database next door.
#
# The files stay in data/ as the upstream artefacts rather than as a wall of hex
# in a committed header.

set(ORGANYA_DATA "${CMAKE_CURRENT_BINARY_DIR}/OrganyaData.cpp")

# Two hex digits to one `0x..,`, then a newline every sixteen bytes so the
# generated file is something a person can open without regretting it.
function(_organya_hex_array out_var path)
    file(READ "${path}" _hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")
    string(REGEX REPLACE "((0x..,){16})" "\\1\n" _bytes "${_bytes}")
    set(${out_var} "${_bytes}" PARENT_SCOPE)
endfunction()

set(ORGANYA_WAVETABLE "${CMAKE_CURRENT_SOURCE_DIR}/data/wavetable.dat")
_organya_hex_array(ORGANYA_WAVETABLE_BYTES "${ORGANYA_WAVETABLE}")
file(SIZE "${ORGANYA_WAVETABLE}" ORGANYA_WAVETABLE_SIZE)

# `extern` on every definition, and it is load-bearing rather than tidy: a
# namespace-scope `const` has *internal* linkage in C++, and extern "C" changes
# the mangling without changing that -- so without it each array is a file-local
# constant nothing else can see, and the compiler says so with an unused-variable
# warning while the link fails.
set(_body "// Generated from codecs/organya/data by CMakeLists.txt. Do not edit.\n")
string(APPEND _body "extern \"C\" {\n\n")
string(APPEND _body "extern const unsigned char kOrganyaWaveTable[];\n")
string(APPEND _body "extern const unsigned int kOrganyaWaveTableSize;\n")
string(APPEND _body "extern const unsigned char kOrganyaWaveTable[] = {\n${ORGANYA_WAVETABLE_BYTES}\n};\n")
string(APPEND _body "extern const unsigned int kOrganyaWaveTableSize = ${ORGANYA_WAVETABLE_SIZE}u;\n\n")

# Globbed rather than listed, and the drum table in OrganyaSynth.cpp looks each
# one up by the number in its name -- so the set is defined by what is in data/
# and the order they arrive in never matters.
file(GLOB ORGANYA_PXT CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/fx*.pxt")
list(SORT ORGANYA_PXT)
list(LENGTH ORGANYA_PXT ORGANYA_PXT_COUNT)
if(ORGANYA_PXT_COUNT EQUAL 0)
    message(FATAL_ERROR "codecs/organya: no fx*.pxt in data/ -- percussion would be silent")
endif()

set(_pointers "")
set(_ids "")
set(_sizes "")
set(_index 0)
foreach(_pxt IN LISTS ORGANYA_PXT)
    get_filename_component(_name "${_pxt}" NAME_WE)
    # fx96 -> 0x96. The name is the patch number the format asks for.
    if(NOT _name MATCHES "^fx([0-9a-fA-F]+)$")
        message(FATAL_ERROR "codecs/organya: ${_name}.pxt is not named fx<hex>")
    endif()
    set(_id "0x${CMAKE_MATCH_1}")

    _organya_hex_array(_pxt_bytes "${_pxt}")
    file(SIZE "${_pxt}" _pxt_size)

    string(APPEND _body "extern const unsigned char kOrganyaPxtData${_index}[];\n")
    string(APPEND _body "extern const unsigned char kOrganyaPxtData${_index}[] = {\n${_pxt_bytes}\n};\n\n")

    string(APPEND _pointers "    kOrganyaPxtData${_index},\n")
    string(APPEND _ids "    ${_id}u,\n")
    string(APPEND _sizes "    ${_pxt_size}u,\n")
    math(EXPR _index "${_index} + 1")
endforeach()

string(APPEND _body "extern const unsigned int kOrganyaPxtCount;\n")
string(APPEND _body "extern const unsigned int kOrganyaPxtId[];\n")
string(APPEND _body "extern const unsigned char* const kOrganyaPxtData[];\n")
string(APPEND _body "extern const unsigned int kOrganyaPxtSize[];\n\n")
string(APPEND _body "extern const unsigned int kOrganyaPxtCount = ${ORGANYA_PXT_COUNT}u;\n")
string(APPEND _body "extern const unsigned int kOrganyaPxtId[] = {\n${_ids}};\n")
string(APPEND _body "extern const unsigned char* const kOrganyaPxtData[] = {\n${_pointers}};\n")
string(APPEND _body "extern const unsigned int kOrganyaPxtSize[] = {\n${_sizes}};\n\n")
string(APPEND _body "}\n")

file(WRITE "${ORGANYA_DATA}" "${_body}")

# So replaced data is picked up rather than silently kept. The glob above is
# already CONFIGURE_DEPENDS; this covers editing a file that was always there.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ORGANYA_WAVETABLE}" ${ORGANYA_PXT})

xpcog_add_codec(
    NAME     organya
    REGISTER xpcog_register_organya
    SOURCES  OrganyaDecoder.cpp OrganyaSynth.cpp "${ORGANYA_DATA}"
    DEPS     xpcog-codec-common
)
