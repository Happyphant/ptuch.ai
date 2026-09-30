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
# Режим диагностики backend'а: кнопка Diagnostics, диалог снимка и отчёт
# для копирования. По умолчанию — включён в Debug и отключён в Release
# (переключается явно: cmake -DPTUCH_DIAGNOSTICS=ON|OFF). При выключении
# UI-часть вырезается макросом PTUCH_DIAGNOSTICS и исключается из сборки;
# API diagnostics() и метрики остаются (дешёвая инструментация, полезна
# и в журналах Release).
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(PTUCH_DIAGNOSTICS_DEFAULT OFF)
else()
    set(PTUCH_DIAGNOSTICS_DEFAULT ON)
endif()
option(PTUCH_DIAGNOSTICS
       "Backend diagnostics mode (кнопка Diagnostics, диалог, отчёт)"
       ${PTUCH_DIAGNOSTICS_DEFAULT})
if(PTUCH_DIAGNOSTICS)
    add_compile_definitions(PTUCH_DIAGNOSTICS=1)
else()
    add_compile_definitions(PTUCH_DIAGNOSTICS=0)
endif()

# Диалог диагностики собирается только при включённом режиме
# (src/UI/diagnostics_dialog.* — иначе Release без этой UI-части).
set(PTUCH_DIAGNOSTIC_SOURCES)
if(PTUCH_DIAGNOSTICS)
    list(APPEND PTUCH_DIAGNOSTIC_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/diagnostics_dialog.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/diagnostics_dialog.cpp"
    )
endif()

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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.qrc"
    ${PTUCH_DIAGNOSTIC_SOURCES}
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/prompt_builder.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/prompt_builder.h"
    # Документ v1: UTF-8 plain text (чтение/атомарная запись через QSaveFile).
    "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.h"
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

# macOS: ad-hoc-подпись бандла после каждой сборки. Linker-подпись
# внутреннего Mach-O не создаёт печать бандла (_CodeSignature), а
# MTLCompilerService при попытке загрузки модели молча не отвечает
# XPC-клиенту с невалидной подписью бандла — llama-поток зависает
# навсегда в ggml_metal_library_init (см. PERFORMANCE_REVIEW.md).
if(APPLE)
    add_custom_command(TARGET PtuchEditor POST_BUILD
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:PtuchEditor>"
        COMMENT "Ad-hoc codesign PtuchEditor.app"
        VERBATIM)
endif()

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
        # Профили стилей: проверка adapterPath/совместимости адаптеров.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.h"
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
        # настроек через кнопку Settings. Диалог диагностики — только
        # при включённом PTUCH_DIAGNOSTICS (кнопка Diagnostics).
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.cpp"
        # MainWindow создаёт StylePanel (правый dock со стилями).
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.qrc"
        # Модель StylePanel — доменный микшер без виджетов.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.h"
        ${PTUCH_DIAGNOSTIC_SOURCES}
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
        # Документ v1: openFile/saveFileTo пишут через TextFileIO.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.h"
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
        # Тема (Theme) — stylesheet диалога и цвета Test Model.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.qrc"
        # Тесты снимка диагностики — только при включённом режиме
        # PTUCH_DIAGNOSTICS (при выключенной опции слоты QSKIP).
        ${PTUCH_DIAGNOSTIC_SOURCES}
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

    # Тесты стилевых профилей: StyleProfile (встроенные профили —
    # обязательные поля, уникальные id, юридически нейтральные имена,
    # стартовый вес 0) и StyleMixer (санитизация весов, кламп,
    # предсказуемая сумма активных весов — normalize() и нормализация
    # на лету дают 1.0, нулевые/выключенные не попадают в prompt,
    # компактная инструкция не разрастается от числа вызовов).
    # Без Qt6::Widgets: QTEST_GUILESS_MAIN в тесте фиксирует, что
    # StyleMixer не зависит от виджетов (нужен только Qt6::Gui под
    # QColor цвета профиля).
    add_executable(StyleMixerTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/style_mixer_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.h"
        # StyleWeight — мост к GenerationRequest::styleWeights.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/text_generation_backend.h"
    )

    target_include_directories(StyleMixerTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(StyleMixerTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(StyleMixerTests)

    target_link_libraries(StyleMixerTests PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Gui
    )

    add_test(NAME StyleMixer COMMAND StyleMixerTests)

    # Тесты StylePanel: строки каждого профиля (маркер, слайдер 0..100,
    # число, нормализованная доля, чекбокс, сброс), единый сигнал
    # styleMixChanged (ровно один на действие, без циклических
    # обновлений), сумма и нормализация из модели (StyleMixer), Reset
    # Mix одним сигналом, тёмный stylesheet панели.
    add_executable(StylePanelTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/style_panel_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.qrc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.h"
        # StyleWeight — payload сигнала styleMixChanged.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/text_generation_backend.h"
    )

    target_include_directories(StylePanelTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(StylePanelTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(StylePanelTests)

    target_link_libraries(StylePanelTests PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )

    add_test(NAME StylePanel COMMAND StylePanelTests)

    # Тесты PromptBuilder: system/user промпт автодополнения по
    # контракту (только продолжение, без пояснений/кавычек/метакомментариев),
    # контекст ровно один раз и без декораций, сохранение языка
    # документа (ru/en шаблоны + автоопределение), смешанные стили с
    # нормализованными весами, детерминизм структуры, санитизация
    # параметров генерации диапазонами AppSettings.
    # Без Qt6::Widgets и без QSettings: QTEST_GUILESS_MAIN + настройки
    # приходят значениями (только константы диапазонов AppSettings).
    add_executable(PromptBuilderTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/prompt_builder_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/prompt_builder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/prompt_builder.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.h"
        # StyleWeight (style_mixer) + диапазоны параметров (prompt_builder).
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/text_generation_backend.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/settings/app_settings.h"
    )

    target_include_directories(PromptBuilderTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(PromptBuilderTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(PromptBuilderTests)

    target_link_libraries(PromptBuilderTests PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Gui
    )

    add_test(NAME PromptBuilder COMMAND PromptBuilderTests)

    # Тесты файлового слоя документа v1 (UTF-8 plain text):
    # round-trip русского текста байт-в-байт (UTF-8 без BOM), атомарная
    # перезапись QSaveFile, ошибки чтения (нет файла/каталог) и записи
    # (нет каталога), отбрасывание ведущего BOM. Без виджетов:
    # QTEST_GUILESS_MAIN + только Qt6::Core.
    add_executable(TextFileIOTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/text_file_io_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.h"
    )

    target_include_directories(TextFileIOTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(TextFileIOTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(TextFileIOTests)

    target_link_libraries(TextFileIOTests PRIVATE
        Qt6::Test
        Qt6::Core
    )

    add_test(NAME TextFileIO COMMAND TextFileIOTests)

    # Тестовый план (TEST_PLAN.md): сквозные сценарии, которым нужен
    # живой MainWindow, но НЕ фокус окна и НЕ реальная модель/GPU:
    #  - ошибка загрузки модели: битый GGUF из PTUCH_MODEL_PATH ->
    #    статус с причиной + fallback на mock + работающие подсказки;
    #  - закрытие приложения посреди активной генерации -> shutdown
    #    (cancel + отвязка backend) + остановка потоков, поздний ответ
    #    отброшен.
    # Состав источников совпадает с GhostSuggestionTests (MainWindow
    # тянет весь UI-контур); llama грузит только заведомо битый файл.
    add_executable(TestPlanTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_plan_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/suggestion_overlay.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/suggestion_overlay.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.qrc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.h"
        ${PTUCH_DIAGNOSTIC_SOURCES}
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
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.h"
    )

    target_include_directories(TestPlanTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(TestPlanTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(TestPlanTests)

    target_link_libraries(TestPlanTests PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        llama
    )

    add_test(NAME TestPlan COMMAND TestPlanTests)

    # Замеры производительности (PERFORMANCE_REVIEW.md): debounce,
    # сборка prompt'a, сборка запроса, отображение ghost, блокировки
    # UI, отмена, накопление памяти/объектов. Always-on слоты идут в
    # ctest без модели/фокуса; слот реальной модели закрыт гейтом
    # PTUCH_MODEL_TESTS=1 (запуск из каталога проекта: models/ рядом).
    add_executable(PerformanceTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/performance_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/MainWindow.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/suggestion_overlay.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/suggestion_overlay.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/settings_dialog.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/style_panel.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.qrc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/style_mixer.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/prompt_builder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/suggestion/prompt_builder.h"
        ${PTUCH_DIAGNOSTIC_SOURCES}
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
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/llama/llama_backend.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/document/text_file_io.h"
    )

    target_include_directories(PerformanceTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(PerformanceTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(PerformanceTests)

    target_link_libraries(PerformanceTests PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        llama
    )

    add_test(NAME Performance COMMAND PerformanceTests)

    # Тесты темы (src/UI/theme.*): палитра (тёмный фон/светлый текст,
    # accent/secondary, disabled), stylesheet с состояниями
    # hover/pressed/disabled/focus, цвета состояний error/generating,
    # контрастность по WCAG, плоский режим без скруглений/градиентов.
    # Без виджетов: QTEST_GUILESS_MAIN + только Qt6::Core/Gui (цвета
    # и QPalette — из QtGui).
    add_executable(ThemeTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/theme_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UI/theme.qrc"
        # suggestion_controller.h подключается темой только ради
        # перечисления SuggestionController::State — заголовок НЕ в
        # списках источников (без moc: контроллер в цель не входит).
    )

    target_include_directories(ThemeTests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_features(ThemeTests PRIVATE cxx_std_20)
    ptuch_enable_warnings(ThemeTests)

    target_link_libraries(ThemeTests PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Gui
    )

    add_test(NAME Theme COMMAND ThemeTests)
endif()
