# USF: Nintendo 64 rips, on lazyusf2. The first HighlyComplete core.
#
# Unlike the container next door in codecs/psf, this one does register with the
# registry -- there is now something behind `usf` that can decode it.
xpcog_add_codec(
    NAME     usf
    REGISTER xpcog_register_usf
    SOURCES  UsfDecoder.cpp
    DEPS     xpcog-codec-psf vendor-lazyusf2 xpcog-codec-common
)
