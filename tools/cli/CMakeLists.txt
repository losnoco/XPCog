# xpcog-cli deliberately links no UI toolkit. That is not incidental: it is the
# structural enforcement of the toolkit-free core rule. If wx -- or anything else
# a display needs -- ever leaks into xpcog-core, this target stops linking and CI
# goes red. cmake/CheckNoToolkit.cmake says the same thing earlier and with a
# better message; this is the one that cannot be forgotten to run.

add_executable(xpcog-cli main.cpp)
target_link_libraries(xpcog-cli PRIVATE xpcog-core xpcog-codecs XPCog::warnings)
set_target_properties(xpcog-cli PROPERTIES FOLDER tools)

# Plays MIDI, so it wants the shipped bank beside it.
add_dependencies(xpcog-cli xpcog-assets)

# Installed alongside the player on Linux, for the same reason it exists in the
# build tree: it is the headless way to ask what an installed XPCog can decode.
# It finds the shipped bank through the same <exe dir>/../share/xpcog lookup the
# application does, so being in the same bin/ is all it needs.
if(UNIX AND NOT APPLE)
    install(TARGETS xpcog-cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    xpcog_install_linux_runtime(xpcog-cli)
endif()
