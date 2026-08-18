# psflib, from Cog Frameworks/psflib (Neill Corlett's PSF format, kode54's
# library). Vendored rather than made an overlay port: four files, and Cog's
# copy is the one the rest of the port is written against.
#
# This is the *container* every PSF variant shares, and the reason it is worth
# having on its own. A .minigsf is a few hundred bytes of override against a
# .gsflib holding the game's whole program; psf_load walks that chain, inflates
# each program section, and hands them over highest-priority-first. Eight
# emulator cores sit on top of exactly this, so it lands once.
#
# zlib because the program section is deflated. Warnings off: this is upstream C
# and the tree is warning-free by policy, which is a statement about our code.
find_package(ZLIB REQUIRED)

# Sources kept under psflib/ because psf2fs.h includes <psflib/psflib.h>:
# Cog's layout, and vendored code is not worth editing to flatten a directory.
add_library(vendor-psflib STATIC psflib/psflib.c psflib/psf2fs.c)
target_include_directories(vendor-psflib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vendor-psflib PUBLIC ZLIB::ZLIB)
set_target_properties(vendor-psflib PROPERTIES FOLDER vendor)
if(MSVC)
    # psflib uses strcasecmp and friends; MSVC spells them _stricmp and warns
    # about the POSIX names rather than the functions being missing.
    target_compile_definitions(vendor-psflib PRIVATE _CRT_NONSTDC_NO_WARNINGS
                                                     _CRT_SECURE_NO_WARNINGS)
    target_compile_options(vendor-psflib PRIVATE /w)
else()
    target_compile_options(vendor-psflib PRIVATE -w)
endif()
