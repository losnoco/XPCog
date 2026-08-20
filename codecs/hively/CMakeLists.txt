# The replayer is vendored rather than packaged -- see vendor/hively for why --
# so there is no find_package() here, only the in-tree target.
xpcog_add_codec(
    NAME     hively
    REGISTER xpcog_register_hively
    SOURCES  HivelyDecoder.cpp
    DEPS     vendor-hively xpcog-codec-common
)
