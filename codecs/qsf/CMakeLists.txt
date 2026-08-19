# QSF: Capcom CPS-2 arcade rips, on HighlyQuixotic.
xpcog_add_codec(
    NAME     qsf
    REGISTER xpcog_register_qsf
    SOURCES  QsfDecoder.cpp
    DEPS     xpcog-codec-psf vendor-highlyquixotic xpcog-codec-common
)
