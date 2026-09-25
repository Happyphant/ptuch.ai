cmake_minimum_required(VERSION 3.16)

project(PtuchEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Конфигурация llama.cpp (БЕЗ глобального AUTOMOC)
set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)

if(APPLE)
    set(GGML_METAL ON CACHE BOOL "" FORCE)
endif()

if(WIN32)
    option(PTUCH_ENABLE_CUDA "Enable CUDA backend" ON)
    if(PTUCH_ENABLE_CUDA)
        set(GGML_CUDA ON CACHE BOOL "" FORCE)
    endif()
endif()

# Добавляем llama.cpp ДО включения AUTOMOC, чтобы избежать предупреждений
add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)

# --- Настройка Qt ---
if(NOT DEFINED CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "/Users/alexanderrabinov/Workspace/Qt/6.11.2-arm64")
endif()

# Включаем автоматическую генерацию Qt файлов ПОСЛЕ добавления llama.cpp
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# Создаем таргет, ЯВНО указав ${CMAKE_CURRENT_SOURCE_DIR} для каждого файла
qt_add_executable(PtuchEditor
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/suggestion_controller.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/suggestion_controller.h"
)

# Устанавливаем свойства таргета
set_target_properties(PtuchEditor PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# Линкуем библиотеки
target_link_libraries(PtuchEditor PRIVATE
    Qt6::Widgets
    llama
)
