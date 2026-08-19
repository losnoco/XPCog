# PSF and PSF2: PlayStation and PlayStation 2 rips, on HighlyExperimental.
#
# vendor-psflib directly, and not only through xpcog-codec-psf: PSF2's sections
# are a filesystem rather than an executable, and psf2fs is what assembles it.
# That file has been vendored since stage 0 waiting for this core.
xpcog_add_codec(
    NAME     psx
    REGISTER xpcog_register_psx
    SOURCES  PsxDecoder.cpp
    DEPS     xpcog-codec-psf vendor-highlyexperimental vendor-psflib
             xpcog-codec-common
)
