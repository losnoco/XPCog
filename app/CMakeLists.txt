find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(xpcog-app
    src/main.cpp
    src/windows/MainWindow.cpp
    src/windows/MainWindow.hpp
)

set_target_properties(xpcog-app PROPERTIES
    OUTPUT_NAME "XPCog"
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME "XPCog"
    MACOSX_BUNDLE_GUI_IDENTIFIER "co.losno.xpcog"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    FOLDER app)

target_link_libraries(xpcog-app PRIVATE
    xpcog-core
    xpcog-codecs
    xpcog-platform
    Qt6::Widgets
    XPCog::warnings)
