cmake_minimum_required(VERSION 3.16)

project(PtuchEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt install prefix (build tree produced from /Users/alexanderrabinov/Workspace/Qt)
if(NOT DEFINED CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "/Users/alexanderrabinov/Workspace/Qt/6.11.2-arm64")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(PtuchEditor
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

target_link_libraries(PtuchEditor PRIVATE Qt6::Widgets)

set_target_properties(PtuchEditor PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
