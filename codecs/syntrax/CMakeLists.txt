# The replayer is vendored rather than packaged -- see vendor/syntrax for why --
# so there is no find_package() here, only the in-tree target.
xpcog_add_codec(
    NAME     syntrax
    REGISTER xpcog_register_syntrax
    SOURCES  SyntraxDecoder.cpp
    DEPS     vendor-syntrax xpcog-codec-common
)
