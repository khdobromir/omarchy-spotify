cmake_minimum_required(VERSION 3.25)
project(omarchy-spotify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(omarchy-spotify
    src/main.cpp
    src/backend.cpp src/backend.h
    src/playback.cpp src/playback.h
    src/theme.cpp src/theme.h
    src/translator.cpp src/translator.h
)
target_link_libraries(omarchy-spotify PRIVATE
    Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2
)
target_include_directories(omarchy-spotify PRIVATE src)
target_compile_options(omarchy-spotify PRIVATE -Wall -Wextra)

set_source_files_properties(src/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
qt_add_qml_module(omarchy-spotify
    URI OmarchySpotify
    QML_FILES src/Main.qml
)
set_source_files_properties(../spotify.py PROPERTIES QT_RESOURCE_ALIAS spotify.py)
qt_add_resources(omarchy-spotify "spotify-helper" PREFIX "/" FILES ../spotify.py)
qt_add_resources(omarchy-spotify "translations" PREFIX "/" BASE src FILES src/i18n/en.json src/i18n/ru.json)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    qt_add_executable(playback-test tests/playback_test.cpp src/playback.cpp)
    target_include_directories(playback-test PRIVATE src)
    target_link_libraries(playback-test PRIVATE Qt6::Test)
    add_test(NAME playback-test COMMAND playback-test)
endif()

install(TARGETS omarchy-spotify RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/omarchy-spotify.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES TEMPLATE-LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/omarchy-spotify)
