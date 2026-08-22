# CONFIG mode is vcpkg's: a distribution's libcurl installs libcurl.pc and, on
# most of them, no CURLConfig.cmake -- so the system branch reads the .pc and
# names the target CURL::libcurl, which is what CONFIG mode would have given.
xpcog_system_dep(curl _curl_from_system)
if(NOT _curl_from_system)
    find_package(CURL CONFIG REQUIRED GLOBAL)
endif()

xpcog_add_codec(
    NAME     httpsource
    REGISTER xpcog_register_httpsource
    SOURCES  HttpSource.cpp IcyDemux.cpp StreamBuffer.cpp
    DEPS     CURL::libcurl xpcog-codec-common
)
