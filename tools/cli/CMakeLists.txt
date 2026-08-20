# xpcog-cli deliberately links no Qt. That is not incidental: it is the structural
# enforcement of the Qt-free core rule. If QtCore ever leaks into xpcog-core, this
# target stops linking and CI goes red.

add_executable(xpcog-cli main.cpp)
target_link_libraries(xpcog-cli PRIVATE xpcog-core xpcog-codecs XPCog::warnings)
set_target_properties(xpcog-cli PROPERTIES FOLDER tools)

# Plays MIDI, so it wants the shipped bank beside it.
add_dependencies(xpcog-cli xpcog-assets)
