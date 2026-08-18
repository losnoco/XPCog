find_package(CURL CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     httpsource
    REGISTER xpcog_register_httpsource
    SOURCES  HttpSource.cpp IcyDemux.cpp StreamBuffer.cpp
    DEPS     CURL::libcurl xpcog-codec-common
)
