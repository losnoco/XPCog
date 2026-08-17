find_package(mpg123 CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     mpg123
    REGISTER xpcog_register_mpg123
    SOURCES  Mpg123Decoder.cpp
    DEPS     MPG123::libmpg123
)
