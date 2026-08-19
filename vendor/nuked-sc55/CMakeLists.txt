# Nuked SC-55: a Roland SC-55mkII, emulated at the chip.
#
# Copied from Cog Frameworks/nuked-sc55, keeping its directory name. Vendored
# rather than made an overlay port by the rule in ../../ports/README.md: this is
# Cog's own tree, carrying an `api.h` that upstream Nuked-SC55 does not have --
# a C entry point over what is otherwise a standalone emulator with its own
# audio output and window.
#
# Seven sources and no shortcuts anywhere in them. This is not a synthesiser
# modelled on an SC-55; it is a Hitachi H8/532 running Roland's own firmware,
# with the sub-MCU, the timers, the interrupt controller, the PCM chip and the
# front-panel LCD all emulated beside it. That is why it needs the ROMs: there
# is nothing to synthesise with until the machine has booted its own code.
#
# Two consequences the layer above has to respect, both of them in api.h:
#
#   The sample rate is the hardware's. sc55_get_sample_rate() has to boot a
#   machine to answer, and the answer is not negotiable -- there is no rate
#   argument anywhere. So `synthSampleRate` does not apply to this synth.
#
#   The LCD is part of the emulation. sc55_render_with_lcd() delivers opaque
#   panel state as it changes, timestamped in milliseconds since boot, and
#   sc55_lcd_render_screen() composites that against data/back.data into RGBA.
#   That file is a raw 741x268 image of the front panel -- exactly
#   741 * 268 * 4 bytes -- and is the one asset here that can be shipped, since
#   Roland's firmware is not.
add_library(vendor-nuked-sc55 STATIC
    nuked-sc55/lcd.cpp
    nuked-sc55/mcu.cpp
    nuked-sc55/mcu_interrupt.cpp
    nuked-sc55/mcu_opcodes.cpp
    nuked-sc55/mcu_timer.cpp
    nuked-sc55/pcm.cpp
    nuked-sc55/submcu.cpp)

# SYSTEM for consumers, so third-party headers are not read under XPCog's own
# warning flags -- the tree being warning-free is a statement about our code.
target_include_directories(vendor-nuked-sc55 SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/nuked-sc55")
target_compile_features(vendor-nuked-sc55 PUBLIC cxx_std_17)
set_target_properties(vendor-nuked-sc55 PROPERTIES FOLDER vendor)

# Local changes to the vendored sources
# -------------------------------------
# None to the sources themselves. The one change is force-included instead, so
# they stay byte-identical to Cog's copy and the next update is a copy rather
# than a merge:
#
#   nuked-sc55/xpcog_quiet.h   defines printf away
#
# The emulator narrates to stdout -- the romset it detected, and then every
# register write the emulation does not model, which the mkII's boot code does
# on its way up. Fine for a standalone emulator with a terminal; not fine inside
# a player, where it lands in whatever stdout happens to be.
set(_sc55_quiet "${CMAKE_CURRENT_SOURCE_DIR}/nuked-sc55/xpcog_quiet.h")

if(MSVC)
    target_compile_definitions(vendor-nuked-sc55 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(vendor-nuked-sc55 PRIVATE /w "/FI${_sc55_quiet}")
else()
    target_compile_options(vendor-nuked-sc55 PRIVATE -w -include "${_sc55_quiet}")
endif()

# Where the front-panel image lives, for whoever draws the LCD. Passed as a
# path rather than embedded here: it is 776 KiB of pixels, and the codec that
# wants it can decide whether to read it once or not at all.
set(XPCOG_SC55_BACKGROUND "${CMAKE_CURRENT_SOURCE_DIR}/data/back.data"
    CACHE INTERNAL "Raw RGBA front panel image for the SC-55 LCD")
