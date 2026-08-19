# HighlyQuixotic: the Capcom CPS-2 QSound core behind QSF.
#
# The smallest of the eight and the only arcade one. A Z80 runs the sound
# program out of a 256 KB ROM and drives the QSound DSP, which reads PCM from a
# sample ROM of up to 8 MB. Both ROMs arrive as sections of the QSF itself --
# the format carries no executable, only ROM images and a byte or two of song
# select -- so unlike every other core here there is no BIOS and nothing to
# load from disk.
#
# ---------------------------------------------------------------------------
# Kabuki
# ---------------------------------------------------------------------------
# CPS-1.5 and CPS-2 sound boards shipped the Z80 as a Kabuki, a custom part
# that decrypts opcodes on the fly with a per-game key. `kabuki.c` reproduces
# it, and the Z80 core keeps a separate opcode map so a decrypted mirror can
# sit under the first 32K while data reads still see the ciphertext.
#
# A rip carries the key in an optional 11-byte `KEY` section. Rips of games
# whose ROMs were already decrypted omit it, and the decoder then hands the
# core a zero key, which is upstream's own signal to leave the ROM alone.
#
# ---------------------------------------------------------------------------
# Local changes to the vendored sources
# ---------------------------------------------------------------------------
#   Core/qmix.c, Core/qmix.h  not vendored. The original QSound mixer,
#                       superseded by qsound_ctr.c -- a DSP emulation derived
#                       from the decapped chip's own microcode. Cog keeps both
#                       files but compiles only qsound_ctr.c, and nothing
#                       includes qmix.h any more.
add_library(vendor-highlyquixotic STATIC
    Core/kabuki.c
    Core/qsound.c
    Core/qsound_ctr.c
    Core/z80.c)

target_include_directories(vendor-highlyquixotic SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(vendor-highlyquixotic PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Core"
    "${CMAKE_CURRENT_SOURCE_DIR}/HighlyQuixotic")
set_target_properties(vendor-highlyquixotic PROPERTIES FOLDER vendor)

# Cog's defines, from its Xcode project.
target_compile_definitions(vendor-highlyquixotic PRIVATE
    EMU_COMPILE EMU_LITTLE_ENDIAN HAVE_STDINT_H)

if(MSVC)
    target_compile_definitions(vendor-highlyquixotic PRIVATE
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX
        WIN32_LEAN_AND_MEAN)
    target_compile_options(vendor-highlyquixotic PRIVATE /w)
else()
    target_compile_options(vendor-highlyquixotic PRIVATE -w)
endif()
