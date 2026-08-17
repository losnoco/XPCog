# An INTERFACE target carrying the project's warning and sanitizer settings.
# Link it PRIVATE from first-party targets only -- never from vendored code.

add_library(xpcog-warnings INTERFACE)
add_library(XPCog::warnings ALIAS xpcog-warnings)

if(MSVC)
    target_compile_options(xpcog-warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    target_compile_definitions(xpcog-warnings INTERFACE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(xpcog-warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wcast-qual -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
        -Wdouble-promotion -Wformat=2)
endif()

if(XPCOG_SANITIZE)
    string(REPLACE ";" "," _san "${XPCOG_SANITIZE}")
    if(MSVC)
        message(WARNING "XPCOG_SANITIZE is not wired up for MSVC; ignoring.")
    else()
        target_compile_options(xpcog-warnings INTERFACE
            -fsanitize=${_san} -fno-omit-frame-pointer)
        target_link_options(xpcog-warnings INTERFACE -fsanitize=${_san})
    endif()
endif()
