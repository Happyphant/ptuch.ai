# Ptuch Editor

Qt 6 desktop-текстовый редактор с AI-подсказками (MVP).

## Требования

- **C++20**
- **Qt 6** (Widgets), собран в `/Users/alexanderrabinov/Workspace/Qt/6.11.2-arm64`
- **CMake ≥ 3.16** (рекомендуется Ninja)
- **llama.cpp** — лежит в `third_party/llama.cpp` и подключается через `add_subdirectory`

## Возможности (текущее состояние)

- **Центральный виджет** — `QPlainTextEdit` (`setCentralWidget`).
- **Верхняя панель состояния** — `QToolBar`: индикатор состояния
  (Ready / Waiting / Generating / Error), кнопки **Generate** и
  **Clear Suggestion**, `QLineEdit` и `QComboBox`; компоновка через
  layouts (без абсолютного позиционирования).
- **Статусная строка** (`QStatusBar`) с описанием текущего события.
- Тёмная тема через `setStyleSheet` в `MainWindow::applyStyle()`.
- Слой подсказок MVP: `SuggestionController` + `PlainTextEditorAdapter`
  (доступ к тексту/курсору редактора) + абстракция `ITextGenerationBackend`
  с mock-реализацией `MockTextGenerationBackend` (асинхронная «генерация»
  на `QTimer` **в отдельном потоке**, без модели и без блокировки UI).
  - debounce 500 мс после паузы в наборе;
  - **ghost-подсказка** рисуется `SuggestionOverlay` поверх viewport'а
    редактора: полупрозрачный текст у курсора (первая строка — сразу за
    курсором, последующие — по левому краю; длинный текст переносится
    по ширине viewport и **не выезжает за правый край экрана**),
    документ при этом не меняется до принятия;
  - **Tab** — принять подсказку (`acceptSuggestion`), **Shift+Tab** —
    альтернатива, **Escape** — отклонить и отменить запрос
    (`rejectSuggestion`); перевод клавиш — в `MainWindow::eventFilter`,
    контроллер виджетов не видит; Tab без показанной подсказки
    не перехватывается (вставляется табуляция); ввод символа и движение
    курсора очищают подсказку;
  - сигналы `suggestionStarted/Ready/Failed/Cleared` и `stateChanged`;
  - явный `requestId` в каждом запросе: устаревшие ответы backend'а
    отбрасываются, даже если тот игнорирует `cancel()`;
  - DI: mock меняется на реальный backend через
    `SuggestionController::setBackend()` без изменения потребителя;
  - `MainWindow` не содержит логики inference — только wiring сигналов.
- **Реальный backend llama.cpp** — `LlamaBackend` реализует
  `ITextGenerationBackend` и подставляется через тот же `setBackend()`
  (DI-точка), как только модель загружена:
  - путь к GGUF: `PTUCH_MODEL_PATH` (файл) / `PTUCH_MODEL_DIR` (папка)
    → настройки `QSettings` (орг. `PtuchAI`, ключ `llama/modelPath`) →
    сканирование папки `models/` проекта;
  - модель загружается **один раз** queued-вызовом в отдельном потоке
    `llamaWorker` (UI не блокируется, sleep в UI нет); при
    отсутствии/ошибке загрузки остаётся mock-бэкенд (fallback +
    сообщение в статусной строке);
  - inference (`llama_decode`, sampling) — **только** в worker-потоке:
    `generate()` из UI переносится queued-событием, `cancel()` —
    атомарный флаг, который читает цикл генерации между чанками
    prompt processing и токенами;
  - `requestId` + отмена: устаревший запрос не стартует, отменённый
    возвращает `stopReason="cancelled"`; потребитель сверяет
    `requestId` независимо;
  - `llama_model`/`llama_context` — RAII (освобождение в потоке
    объекта при закрытии: `closeEvent → stopLlamaWorker`), один
    контекст на объект и строго последовательная обработка запросов —
    параллельного доступа к контексту нет;
  - лимиты: `n_ctx` зажат (256..8192 и не больше контекста обучения),
    промпт обрезается под `n_ctx` (сверху, ближе к курсору), генерация
    ≤ 512 токенов; в UI — только `ITextGenerationBackend`;
  - логирование категорией `ptuch.llama`: путь и время загрузки,
    `n_ctx`/потоки, число токенов промпта, время prompt processing,
    время генерации, причина остановки, поток выполнения.
- **Диалог настроек** — `SettingsDialog` (кнопка **Settings** в панели,
  хранение `QSettings`, ключи/дефолты/диапазоны — `AppSettings`):
  - поля: путь к GGUF-модели (кнопка выбора файла `…`, пусто —
    авто-поиск как при старте), размер контекста, максимум новых
    токенов, temperature, top-p, число GPU-слоёв (−1 = «все слои»),
    интервал debounce и переключатель автоматических подсказок;
  - **проверка диапазонов**: `QSpinBox`/`QDoubleSpinBox` ограничены
    диапазонами `AppSettings`, а `AppSettings::load()` дополнительно
    санирует правленый руками файл настроек — некорректное значение
    не доедет ни до контроллера, ни до `LlamaBackend`
    (защита в глубину, см. `app_settings.h`);
  - кнопка **Test Model**: загрузка GGUF на **временном** `LlamaBackend`
    в собственном потоке (приложение продолжает генерировать на своём
    backend'е — активная генерация не отменяется и модель не
    перезагружается); прогресс/успех/ошибка загрузки показываются
    цветной меткой рядом с кнопкой, поток останавливается
    `requestStop → quit → wait` (в т.ч. в деструкторе диалога);
  - настройки сохраняются между запусками (`save()` + `sync()`);
  - **обновление не разрушает активную генерацию**: параметры
    генерации/debounce/авто применяются сразу (контроллер копирует их
    в каждый новый запрос, запущенный debounce-таймер не сбрасывается,
    выключение авто не отменяет запрос «в полёте» и не блокирует
    ручной `requestSuggestion`); перезагрузка модели
    (`MainWindow::reloadLlamaBackend`) — только при реальном
    изменении пути/контекста/GPU-слоёв и безопасным свапом
    (контроллер → mock: cancel + bump id → остановка старого потока →
    новый backend с новыми настройками, пока он грузится — mock).
- **Диагностика backend'а** — кнопка **Diagnostics** в панели →
  `DiagnosticsDialog` со снимком активного backend'а:
  - поля: имя backend'а, имя модели (только файл GGUF), размер
    контекста, число GPU-слоёв, prompt/generated токены, время
    обработки prompt'а, время генерации, токенов в секунду и
    последняя ошибка; метрики — последнего **успешного** запроса,
    пустые поля показываются как «—»;
  - кнопка **Копировать** кладёт текстовый отчёт в буфер обмена;
    отчёт формируется строго из полей снимка — **без указателей и
    внутренних адресов** (контролируется тестом);
  - снимок фиксируется при открытии диалога; данные собирает сам
    backend — `ITextGenerationBackend::diagnostics()`,
    потокобезопасная копия под мьютексом (UI не блокируется);
  - **режим отключаем**: опция `PTUCH_DIAGNOSTICS` (по умолчанию ON
    в Debug, OFF в Release) вырезает кнопку, диалог и их тесты —
    см. «Сборка»; API `diagnostics()` и метрики остаются.

## Структура

```
CMakeLists.txt
src/
  main.cpp                      # composition root: QApplication, имена для QSettings;
                                # потоки backend'ов заводит MainWindow
  UI/
    MainWindow.h / .cpp         # central QPlainTextEdit, toolbar, статусная строка, wiring
                                # (mock + llama: setupLlamaBackend/stopLlamaWorker;
                                #  настройки: openSettings/applySettings/reloadLlamaBackend;
                                #  диагностика: openDiagnostics — под PTUCH_DIAGNOSTICS)
    suggestion_overlay.h / .cpp # ghost-подсказка поверх viewport'а (документ не трогает)
    settings_dialog.h / .cpp    # SettingsDialog: поля с диапазонами, выбор файла,
                                # Test Model на временном LlamaBackend (свой поток)
    diagnostics_dialog.h / .cpp # DiagnosticsDialog: снимок BackendDiagnostics + копирование
                                # отчёта в буфер (сборка только при PTUCH_DIAGNOSTICS=ON)
  settings/
    app_settings.h / .cpp       # AppSettings: единый источник ключей/дефолтов/диапазонов
                                # QSettings + sanitize() (защита от мусора)
  llama/
    llama_backend.h / .cpp      # LlamaBackend: ITextGenerationBackend поверх llama.cpp
                                # (pimpl, worker-поток, RAII, отмена, логи ptuch.llama)
  backend/
    text_generation_backend.h   # ITextGenerationBackend + GenerationRequest/Result
                                # + BackendDiagnostics (снимок для диагностики)
    mock_text_generation_backend.h/.cpp # mock: задержка, ошибка, отмена,
                                 # живёт в отдельном (не UI) потоке
  suggestion/
    suggestion_controller.h/.cpp # debounce, generation id, сигналы
                                 # suggestion*/stateChanged (без виджетов)
    document_state.h/.cpp       # снимок документа + безопасный контекст (обрезка)
    editor_adapter.h/.cpp       # QPlainTextEdit -> ISuggestionEditor
tests/
  document_state_test.cpp       # Qt Test: DocumentState
  suggestion_controller_test.cpp # Qt Test: debounce, устаревшие ответы
  mock_text_generation_backend_test.cpp # Qt Test: поток, ошибка, отмена
  ghost_suggestion_test.cpp    # Qt Test: overlay (пиксели) + клавиши Tab/Escape
                               # + жизненный цикл MainWindow с реальной моделью
  llama_backend_test.cpp       # Qt Test: контракт LlamaBackend + smoke с GGUF
  settings_test.cpp            # Qt Test: AppSettings (дефолты/roundtrip/санитизация)
                               # + SettingsDialog (диапазоны, ошибка Test Model)
                               # + диагностика (отчёт, диалог, буфер обмена)
models/                         # *.gguf (в .gitignore)
third_party/
  llama.cpp/                    # вендор (не редактируется)
```

## Сборка

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/Users/alexanderrabinov/Workspace/Qt/6.11.2-arm64 \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build -j
```

При желании Ninja можно заменить на Makefiles — флаг `-G Ninja` просто убирается.

### Предупреждения

Для кода проекта включены `-Wall -Wextra` (MSVC: `/W4`).
На third_party (`llama.cpp`, `ggml`) предупреждения **не** распространяются.

### Режим диагностики (PTUCH_DIAGNOSTICS)

Кнопка **Diagnostics**, `diagnostics_dialog.*` и тесты диагностики
собираются под опцией `PTUCH_DIAGNOSTICS` — по умолчанию `ON` в Debug
и `OFF` в Release (переключается явно):

```bash
cmake -S . -B build ... -DPTUCH_DIAGNOSTICS=OFF  # выключить в Debug
cmake -S . -B build ... -DPTUCH_DIAGNOSTICS=ON   # включить в Release
```

При выключении UI-часть вырезается полностью: файлы диалога не
включаются в сборку, кнопка не создаётся, слоты тестов уходят в
`QSKIP`. `ITextGenerationBackend::diagnostics()` и сбор метрик
остаются (дешёвая инструментация — полезна и в журналах Release).

## Тесты

Unit-тесты на **Qt Test** (входит в Qt 6, внешних зависимостей нет):

```bash
ctest --test-dir build --output-on-failure
# или напрямую
./build/DocumentStateTests
./build/SuggestionControllerTests
./build/MockTextGenerationBackendTests
./build/GhostSuggestionTests
./build/LlamaBackendTests
./build/SettingsTests
```

Медленные проверки с реальной моделью (2 ГБ GGUF в `models/`) в `ctest`
по умолчанию **не** входят — включаются переменной окружения:

```bash
PTUCH_MODEL_TESTS=1 ./build/LlamaBackendTests          # smoke: загрузка, генерация, отмена
PTUCH_MODEL_TESTS=1 ./build/GhostSuggestionTests        # + жизненный цикл MainWindow с моделью
PTUCH_MODEL_TESTS=1 ./build/SettingsTests               # + Test Model в диалоге с реальной GGUF
```

Покрытие:

- `DocumentState` — пустой документ, курсор в начале/середине,
  большой текст (обрезка префикса/суффикса, жёсткий потолок размера),
  зажим курсора в границах, смена generation id, исключение ghost-подсказки
  из контекста.
- `SuggestionController` — debounce (непрерывная печать не порождает
  запросов, после паузы ровно один), пустой контекст без генерации,
  мгновенный ручной `requestSuggestion` (и параметры запроса), рост
  generation id, игнорирование устаревшего ответа, отмена по
  `rejectSuggestion`, ошибки backend'а, `acceptSuggestion`, `setStyleMix`;
  обновление настроек «на лету»: `setDebounceInterval` (кламп
  границами, новый интервал применяется к следующему таймеру),
  `setAutoSuggestions` (гейт только debounce-пути: хвост таймера
  гаснет, ручной запрос и активная генерация живут) и
  `setGenerationParams` посреди активной генерации (запрос не
  отменяется, завершается со своим id, новые параметры уходят в
  следующий запрос). Тесты идут без виджетов — только через интерфейсы.
- `MockTextGenerationBackend` — работа в отдельном потоке (не в
  UI-потоке), задержка и `elapsedMs`, явный `requestId` у конкурентных
  запросов, имитация ошибки, отмена запроса (честный режим и режим
  `ignoreCancel` — «плохой» backend отвечает на отменённый запрос).
- `GhostSuggestion` — overlay рисует ghost у курсора (пиксельная проверка
  рендера: положение рядом с курсором, полупрозрачность alpha ≤ 130,
  документ не изменён), многострочные подсказки (вторая строка по левому
  краю, лишних строк нет), мышь/фокус overlay прозрачны, а также полный
  сценарий клавиш в `MainWindow`: печать → подсказка, **Tab** принимает
  (текст в документе), Tab без подсказки проходит в редактор (вставляется
  `\t`), ввод символа и движение курсора чистят подсказку, **Escape**
  отклоняет без изменения документа. Слот
  `settingsDialogAppliesAndPersists` открывает диалог кнопкой
  **Settings**, меняет debounce/авто и проверяет: контроллер получил
  новые значения, `QSettings` сохранил их. Слот
  `diagnosticsButtonOpensDialog` (при `PTUCH_DIAGNOSTICS=ON`, иначе
  `QSKIP`) открывает диалог кнопкой **Diagnostics**, проверяет снимок
  mock-бэкенда («mock», у модели «—») и копирование отчёта в буфер
  без адресов. Отдельный слот
  `mainWindowLoadsRealModelAndClosesCleanly` (под `PTUCH_MODEL_TESTS=1`)
  грузит реальную GGUF через `MainWindow` и закрывает окно — проверка
  `closeEvent → stopLlamaWorker → RAII`-освобождения.
- `LlamaBackend` — поиск модели (env override, несуществующий путь не
  возвращается), ошибка «модель не загружена» с эхо `requestId`,
  асинхронность `generate()` из чужого потока (queued-диспетчеризация:
  сразу после вызова ответа нет), безопасная отмена неизвестного id.
  Диагностика: `diagnostics()` после `configure()` (имя модели — только
  файл, n_ctx/GPU-слои, пустая ошибка, нулевые метрики) и фиксация
  последней ошибки генерации в снимке. Smoke под `PTUCH_MODEL_TESTS=1`:
  загрузка GGUF в worker-потоке (проверка потока исполнения сигналом),
  генерация (непустой текст, `elapsedMs`, допустимые `stopReason`),
  метрики последнего успешного запроса в снимке (prompt/generated
  токены, времена фаз, пустая ошибка), идемпотентный повтор
  `loadModel()`, отмена → `stopReason="cancelled"`.
- `Settings` — дефолты при пустом `QSettings`, roundtrip
  «сохранил → новый запуск → загрузил» (в т.ч. ключ `llama/modelPath`
  находит `resolveModelPath()`), санитизация вышедших за диапазон и
  нечисловых значений (`load()` возвращает валидные значения),
  заполнение `SettingsDialog` из сохранённых настроек, жёсткие
  диапазоны виджетов (кламп при `setValue`), `settings()` в границах,
  ошибка Test Model: несуществующий файл (мгновенно, без потока) и
  битый GGUF (асинхронная ошибка из тест-потока, UI не зависает).
  Тесты снимают и возвращают только ключи приложения (`llama/*`,
  `suggestion/*`) — чужие ключи файла (fallback `NSGlobalDomain`)
  не затираются и не копируются. Под `PTUCH_MODEL_TESTS=1` — Test Model
  с реальной моделью до состояния `OK:`.
- `Диагностика` (слоты `QSKIP` при `PTUCH_DIAGNOSTICS=OFF`) —
  форматирование отчёта: все поля снимка, посчитанные «токенов в
  секунду», «—» для пустых значений, отсутствие адресов `0x`; диалог:
  метки соответствуют полям снимка, кнопка **Копировать** кладёт отчёт
  в буфер обмена.

Снимок ghost-подсказки для ручного просмотра (по желанию):

```bash
PTUCH_GHOST_SCREENSHOT=/tmp/ptuch_ghost.png ./build/GhostSuggestionTests
open /tmp/ptuch_ghost.png
```

Ручной сценарий проверки клавиш (в запущенном редакторе):

1. Печатаем слово и ждём ~1 сек — после курсора появляется
   полупрозрачная подсказка; документ при этом не изменился.
2. **Tab** — подсказка вставляется в документ, призрак исчезает.
3. Печатаем ещё слово, ждём подсказку, нажимаем **Escape** — подсказка
   исчезает, документ не изменился.
4. Снова ждём подсказку и вводим обычный символ — подсказка гаснет,
   символ попадает в документ.
5. Ждём подсказку и нажимаем стрелку — подсказка гаснет.
6. Без показанной подсказки нажимаем **Tab** — вставляется табуляция
   (клавиша не перехватывается).

Отключается опцией `-DPTUCH_BUILD_TESTS=OFF`.

## Запуск

```bash
./build/PtuchEditor.app/Contents/MacOS/PtuchEditor
# или
open build/PtuchEditor.app
```

### Модель (llama.cpp)

При старте `MainWindow` ищет GGUF и грузит её в фоне (первым
приоритетом — переменная окружения, затем настройки, затем `models/`):

| Источник | Ключ / правило |
| --- | --- |
| Окружение | `PTUCH_MODEL_PATH` (файл) или `PTUCH_MODEL_DIR` (папка с `*.gguf`) |
| Настройки | `QSettings` org `PtuchAI`, app `PtuchEditor`, ключ `llama/modelPath` |
| Папка проекта | первый `*.gguf` в `models/` (cwd, каталог бинаря или бандла) |

- Модель не найдена / не загрузилась — подсказки остаются на mock,
  причина пишется в статусную строку.
- `PTUCH_DISABLE_LLAMA=1` — не искать и не грузить модель (используется
  UI-тестами).
- Логи llama-части (загрузка, токены, тайминги, причины остановки):

```bash
./build/PtuchEditor.app/Contents/MacOS/PtuchEditor 2>&1 | grep ptuch.llama
```

### Настройки (QSettings)

Кнопка **Settings** в верхней панели открывает `SettingsDialog`; OK
применяет и сохраняет значения (единый источник — `AppSettings`,
`src/settings/app_settings.h`):

| Ключ | Дефолт | Диапазон |
| --- | --- | --- |
| `llama/modelPath` | пусто (авто-поиск) | — |
| `llama/contextSize` | 4096 | 256..8192 |
| `llama/maxTokens` | 64 | 1..512 |
| `llama/temperature` | 0.7 | 0.1..2.0 |
| `llama/topP` | 0.9 | 0.1..1.0 |
| `llama/gpuLayers` | −1 (все слои) | −1..128 |
| `suggestion/debounceMs` | 500 | 50..10000 |
| `suggestion/autoSuggestions` | включено | bool |

- Значения вне диапазона (в т.ч. правленый руками файл) обрезаются в
  `AppSettings::load()` — до `LlamaBackend`/контроллера доезжают
  только валидные значения.
- Путь/n_ctx/GPU-слои применяются перезагрузкой модели **только при
  изменении** (безопасным свапом, активная генерация корректно
  завершается как устаревшая); остальные параметры — сразу, без
  отмены запроса.

### Диагностика (кнопка Diagnostics)

Кнопка **Diagnostics** в верхней панели (собирается при
`PTUCH_DIAGNOSTICS=ON`) открывает `DiagnosticsDialog` — снимок
активного backend'а на момент открытия: имя backend'а и модели,
`n_ctx`, GPU-слои, prompt/generated токены, времена фаз,
токенов в секунду и последняя ошибка. Кнопка **Копировать** кладёт
тот же отчёт в буфер обмена; пустые поля отображаются как «—».
Новые цифры — после новых запросов (закрыть и открыть снова).

## Стандарт и автогенерация Qt

- `CMAKE_CXX_STANDARD 20` и `target_compile_features(... cxx_std_20)` —
  применяются только к таргету `PtuchEditor` (llama.cpp остаётся на C++17,
  чтобы не пересобирать third_party).
- `CMAKE_AUTOMOC` / `CMAKE_AUTOUIC` / `CMAKE_AUTORCC` включены явно и
  продублированы свойствами таргета.
- Глобальных mutable-объектов в коде нет: состояние backend'а инкапсулировано
  в pimpl, живущем в своём потоке; метатипы регистрируются в конструкторе.
