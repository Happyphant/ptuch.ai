cmake_minimum_required(VERSION 3.16)

project(PtuchEditor LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Конфигурация llama.cpp.
# Делается ДО find_package(Qt) и ДО включения AUTOMOC: third_party не должен
# попадать под Qt-автогенерацию (moc/uic/rcc).
# ---------------------------------------------------------------------------
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

add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# C++20.
# Переменная ставится ПОСЛЕ add_subdirectory(), поэтому действует только на
# таргеты проекта (PtuchEditor). llama.cpp/ggml остаются на своём cxx_std_17
# (см. third_party/llama.cpp/src/CMakeLists.txt: "don't bump") и не
# пересобираются заново.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Qt 6
# ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "/Users/alexanderrabinov/Workspace/Qt/6.11.2-arm64")
endif()

# Автогенерация Qt-файлов (moc/uic/rcc) — явно, как требование проекта.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# ---------------------------------------------------------------------------
# Исполняемый таргет
# ---------------------------------------------------------------------------
qt_add_executable(PtuchEditor
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/suggestion_overlay.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/suggestion_overlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/settings/app_settings.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/settings/app_settings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/text_generation_backend.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/mock_text_generation_backend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/mock_text_generation_backend.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/suggestion_controller.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/suggestion_controller.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/document_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/document_state.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/editor_adapter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/editor_adapter.h"
)

set_target_properties(PtuchEditor PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    # Страховка: автогенерация включена и на уровне таргета.
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

# Явная привязка таргета к C++20 (не полагаемся только на CMAKE_CXX_STANDARD).
target_compile_features(PtuchEditor PRIVATE cxx_std_20)

# Пути включения: #include "llama/...", "suggestion/...", "UI/..."
target_include_directories(PtuchEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Предупреждения компилятора — только для нашего кода, не для third_party.
function(ptuch_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

ptuch_enable_warnings(PtuchEditor)

target_link_libraries(PtuchEditor PRIVATE
    Qt6::Widgets
    llama
)

# ---------------------------------------------------------------------------
# Unit-тесты (Qt Test входит в Qt 6 — внешних зависимостей не добавляется)
# ---------------------------------------------------------------------------
option(PTUCH_BUILD_TESTS "Build unit tests" ON)

if(PTUCH_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    enable_testing()

    add_executable(DocumentStateTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/document_state_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/document_state.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/document_state.h"
    )

    target_include_directories(DocumentStateTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(DocumentStateTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(DocumentStateTests)

    target_link_libraries(DocumentStateTests PRIVATE
        Qt6::Test
        Qt6::Core
    )

    add_test(NAME DocumentState COMMAND DocumentStateTests)

    # Тесты SuggestionController: debounce, generation id, устаревшие ответы.
    # Контроллер проверяется без виджетов — только через интерфейсы.
    add_executable(SuggestionControllerTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/suggestion_controller_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/suggestion_controller.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/suggestion_controller.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/document_state.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/document_state.h"
        # Мета-объект интерфейса backend'а (сигналы ready/error).
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/text_generation_backend.h"
    )

    target_include_directories(SuggestionControllerTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(SuggestionControllerTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(SuggestionControllerTests)

    # QTextCursor/QTextDocument — из Qt6::Gui (тянет Widgets в приложении).
    target_link_libraries(SuggestionControllerTests PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Gui
    )

    add_test(NAME SuggestionController COMMAND SuggestionControllerTests)

    # Тесты MockTextGenerationBackend: задержка, ошибка, отмена,
    # работа в отдельном потоке.
    add_executable(MockTextGenerationBackendTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_text_generation_backend_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/mock_text_generation_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/mock_text_generation_backend.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/text_generation_backend.h"
    )

    target_include_directories(MockTextGenerationBackendTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(MockTextGenerationBackendTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(MockTextGenerationBackendTests)

    target_link_libraries(MockTextGenerationBackendTests PRIVATE
        Qt6::Test
        Qt6::Core
    )

    add_test(NAME MockBackend COMMAND MockTextGenerationBackendTests)

    # Тесты ghost-подсказки: overlay рисует поверх viewport'а, не меняя
    # документ (проверка пикселями), + сценарий клавиш в MainWindow:
    # Tab принимает, Escape отклоняет, Tab без подсказки не перехватывается,
    # обычный ввод и движение курсора чистят подсказку.
    add_executable(GhostSuggestionTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ghost_suggestion_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/suggestion_overlay.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/suggestion_overlay.cpp"
        # MainWindow открывает SettingsDialog; тест проверяет применение
        # настроек через кнопку Settings.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/settings/app_settings.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/settings/app_settings.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/suggestion_controller.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/suggestion_controller.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/document_state.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/document_state.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/editor_adapter.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/editor_adapter.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/text_generation_backend.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/mock_text_generation_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/mock_text_generation_backend.h"
        # MainWindow создаёт llama-бэкенд; в тестах он отключён через
        # PTUCH_DISABLE_LLAMA (см. GhostSuggestionTest::initTestCase).
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.h"
    )

    target_include_directories(GhostSuggestionTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(GhostSuggestionTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(GhostSuggestionTests)

    target_link_libraries(GhostSuggestionTests PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        llama
    )

    add_test(NAME GhostSuggestion COMMAND GhostSuggestionTests)

    # Тесты LlamaBackend: контракт ITextGenerationBackend без модели
    # (ошибка «модель не загружена», асинхронный вызов из чужого потока,
    # отмена неизвестного id, поиск модели) + опциональный интеграционный
    # smoke с реальной GGUF — включается PTUCH_MODEL_TESTS=1 (в ctest по
    # умолчанию пропускается, чтобы не грузить 2 ГБ модель).
    add_executable(LlamaBackendTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/llama_backend_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/text_generation_backend.h"
    )

    target_include_directories(LlamaBackendTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(LlamaBackendTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(LlamaBackendTests)

    target_link_libraries(LlamaBackendTests PRIVATE
        Qt6::Test
        Qt6::Core
        llama
    )

    add_test(NAME LlamaBackend COMMAND LlamaBackendTests)

    # Тесты настроек: AppSettings (дефолты, сохранение между запусками,
    # санитизация вышедших за диапазон значений — мусор из QSettings не
    # доезжает до LlamaBackend) и SettingsDialog (заполнение из
    # сохранённых значений, жёсткие диапазоны виджетов, отображение
    # ошибки Test Model без зависания UI; путь к модели — во временном
    # каталоге, реальная GGUF не грузится).
    add_executable(SettingsTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/settings_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/settings/app_settings.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/settings/app_settings.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.cpp"
        # Test Model работает с временным LlamaBackend (свой поток).
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/text_generation_backend.h"
    )

    target_include_directories(SettingsTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(SettingsTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(SettingsTests)

    target_link_libraries(SettingsTests PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Widgets
        llama
    )

    add_test(NAME Settings COMMAND SettingsTests)
endif()
