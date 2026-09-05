# xpcog-doctypes writes the document types XPCog.app declares -- see main.cpp
# for what and why. macOS only, and built for the bundle rather than for anyone
# to run: app/CMakeLists.txt runs it after every build and splices what it
# wrote into Info.plist. Like xpcog-cli it links no toolkit, and unlike it this
# is not a layering check, just a program that needs none.
#
# CoreServices is for LaunchServices, which is asked which of the extensions
# macOS already has a type for.

add_executable(xpcog-doctypes main.cpp)
target_link_libraries(xpcog-doctypes PRIVATE
    xpcog-core xpcog-codecs XPCog::warnings "-framework CoreServices")
set_target_properties(xpcog-doctypes PROPERTIES FOLDER tools)
