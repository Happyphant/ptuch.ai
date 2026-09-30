# Тестовый план ПТЮЧ.AI

Единственный фреймворк — **Qt Test** (входит в Qt 6, внешних зависимостей
нет), запуск через `ctest`. План фиксирует, какой пункт покрыт какой
целью и каким слотом, какие тесты добавлены и как обеспечивается
детерминизм.

## Требования к тестам

1. **Qt Test только.** Никаких сторонних фреймворков; `QTEST_MAIN` /
   `QTEST_GUILESS_MAIN` в зависимости от необходимости виджетов.
2. **Детерминизм.** Время задаётся, а не «ждрётся вслепую»:
   `QTest::qWaitFor` с таймаутом на условие; короткие `qWait` — только
   там, где нужно доказать *отсутствие* события. Поведение AI-слоя
   задаётся явно через mock-бэкенд (см. ниже).
3. **Без GPU и без GGUF.** Никакой тест не требует реальной модели:
   - `PTUCH_DISABLE_LLAMA=1` — llama отключается в UI-тестах
     (GhostSuggestion, Settings);
   - ошибка загрузки модели проверяется на **заведомо битом файле**
     (`PTUCH_MODEL_PATH` -> `corrupt.gguf`): падение детерминировано и
     происходит до использования ускорителей;
   - интеграционный smoke с реальной 2 ГБ моделью — только вручную:
     `PTUCH_MODEL_TESTS=1` (в `ctest` по умолчанию не входит).
4. **Фокус macOS.** Слоты, которым нужен живой ввод, активируют окно
   через `activateAndFocus`; сквозные тесты плана фокуса **не требуют**
   (ручной `requestSuggestion` работает и без фокуса), поэтому
   прогон TestPlan устойчив к перехвату окна другими приложениями.
5. **Гигиена QSettings.** Тесты, пишущие в настройки, обёрнуты
   `QSettingsBackup` (снимок + самовосстановление после SIGABRT);
   TestPlanSettings-ключи только читает.

## Mock-бэкенд: детерминированное управление

`MockTextGenerationBackend` (`src/backend/`) — настройки вызываются
**до** запуска потока ( setters не потокобезопасны):

| Что задать | Метод | Эффект |
| --- | --- | --- |
| Задержка | `setDelayMs(ms)` | ответ через ровно N мс (таймер в потоке объекта) |
| Результат | `setResultText(text)` | в ответ уходит ровно заданный текст (пусто — автоподсказка по контексту) |
| Ошибка | `setSimulateError(true)` | `generationError(requestId, текст)` вместо ответа |
| «Плохой» cancel | `setIgnoreCancel(true)` | ответ приходит даже на отменённый запрос — проверка отсечения по `requestId` |

Контроллерный уровень использует синхронный `FakeBackend`
(один поток, ответ по требованию) — гонки воспроизводятся без сна.

## Матрица покрытия

| # | Пункт плана | Цель ctest | Слоты |
| --- | --- | --- | --- |
| 1 | DocumentState | `DocumentState` | `emptyDocument`, `cursorAtStart`/`cursorInMiddle`, `largeTextTruncatesPrefix`/`Suffix`, `customLimitNeverExceedsHardCap`, `selectionTruncated`, `cursorClampedToBounds`, `generationIdChangeInvalidatesOldResponses`, `ghostSuggestionNotIncluded` |
| 2 | StyleMixer | `StyleMixer` | `builtinProfilesExposeRequiredFields`/`DefaultToInactive`, `mixerSanitizesWeightOnConstruction`, `setWeightClampsAndRejectsUnknownId`, `setEnabledFlipsParticipation`, `zeroAndDisabledProfilesStayOutOfPrompt`, `normalizeMakesActiveSumExactlyOne`, `styleWeightsAreNormalizedOnTheFly`, `instructionIsCompactPerActiveProfile`/`DoesNotGrowWithRepeatedCalls` |
| 3 | PromptBuilder | `PromptBuilder` | `noStylesProducesRulesOnlySystemPrompt`, `singleStyleAppendsInstructionBlock`, `twoStylesAppearTogetherWithNormalizedWeights`, `russianContextKeepsLanguage`/`englishContextKeepsLanguage`, `languageAutoDetectsFromContext`, `contextIsNeverDuplicatedOrDecorated`, `structureIsDeterministic`, `generationParamsAreSanitized` |
| 4 | Debounce | `SuggestionController` | `debounceSingleRequestWhileTyping` (печать не порождает запросов, после паузы ровно один), `debounceIntervalApplies` (кламп и тайминг), `manualRequestIsImmediate` (ручной минует debounce) |
| 5 | requestId / generationId | `SuggestionController` + `MockBackend` | `generationIdIncreasesPerRequest`; `concurrentRequestsKeepTheirRequestIds`, **`configuredResultTextIsReturned`** (новый: заданный результат байт-в-байт) |
| 6 | Отмена запроса | `SuggestionController` + `MockBackend` | `rejectCancelsPendingRequest`; `cancelSuppressesResponse`, `ignoreCancelStillResponds` |
| 7 | Игнорирование устаревшего ответа | `SuggestionController` | `staleResponseIgnored`, `raceOfNewRequestCancelAndOldResponse`, `staleErrorDroppedWhileNewRequestActive` |
| 8 | Принятие ghost suggestion | `SuggestionController` + `GhostSuggestion` | `acceptSuggestionInsertsText`; `keysScenario` (Tab принимает показанную подсказку) |
| 9 | Отклонение ghost suggestion | `SuggestionController` + `GhostSuggestion` | `rejectCancelsPendingRequest`; `keysScenario` (Escape отклоняет, документ цел) |
| 10 | Загрузка/сохранение UTF-8 | `TextFileIO` + `GhostSuggestion` | `roundTripsRussianTextAsUtf8`, `overwritesExistingFileAtomically`, `stripsLeadingBomOnLoad`, `loadsMissingFileAsError`/`loadsDirectoryAsError`/`savesToMissingDirectoryAsError`, `savesEmptyText`; `documentOpenShowsNameAndClearsSuggestion`, `documentSaveWritesUtf8AndClearsModified`, `documentNewClearsEditorAndSuggestion` |
| 11 | Ошибка загрузки модели | **`TestPlan`** (новая цель) + `LlamaBackend` + `Settings` | **`modelLoadErrorFallsBackToMockAndSuggestionsStillWork`** (новый: битый GGUF -> статус «остаёмся на mock» -> контроллер на mock -> подсказка Ready -> чистое закрытие); `generate_withoutModel_reportsError` (контракт без модели), `testModelReportsMissingFileWithoutBlocking`/`testModelReportsCorruptGguf` (диалог Test Model) |
| 12 | Закрытие приложения во время генерации | **`TestPlan`** (новая цель) + `SuggestionController` + `GhostSuggestion` | **`closeDuringGenerationStopsWorkerAndDropsLateAnswer`** (новый: closeEvent посреди генерации -> shutdown + поздний ответ отброшен); **`shutdownDuringGenerationDropsLateAnswer`** (новый unit-слот: cancel + отвязка + мёртвость контроллера); `ctrlSpaceAndFocusOutScenario` (UI: закрытие при активной генерации, без крашей) |

Проверки инфраструктуры, не входящие в 12 пунктов, но обязательные по
соглашению проекта: overlay (пиксели, скролл, фон), StylePanel-сигналы,
диалоги Settings/Diagnostics, `mainWindowLoadsRealModelAndClosesCleanly`
(гейт `PTUCH_MODEL_TESTS=1`).

## Новые и изменённые файлы

- `src/backend/mock_text_generation_backend.{h,cpp}` — новый
  `setResultText()` (детерминированный результат).
- `tests/mock_text_generation_backend_test.cpp` — слот
  `configuredResultTextIsReturned`.
- `tests/suggestion_controller_test.cpp` — слот
  `shutdownDuringGenerationDropsLateAnswer`.
- `tests/test_plan_test.cpp` — **новый**: сквозные слоты
  `modelLoadErrorFallsBackToMockAndSuggestionsStillWork`,
  `closeDuringGenerationStopsWorkerAndDropsLateAnswer`
  (`QTEST_MAIN`, окно не показывается — фокус не требуется;
  env `PTUCH_MODEL_PATH` сохраняется/восстанавливается).
- `CMakeLists.txt` — цель **`TestPlanTests`** + `add_test(NAME TestPlan …)`.

## Запуск

```bash
# полный прогон (11 целей)
ctest --test-dir build --output-on-failure

# только план
ctest --test-dir build -R "TestPlan" --output-on-failure
./build/TestPlanTests

# отдельные цели
./build/SuggestionControllerTests
./build/MockTextGenerationBackendTests
./build/TextFileIOTests

# медленные проверки с реальной моделью (в ctest не входят)
PTUCH_MODEL_TESTS=1 ./build/LlamaBackendTests
PTUCH_MODEL_TESTS=1 ./build/GhostSuggestionTests
```

Критерий приёмки: все 11 целей зелёные в Debug и Release, без
предупреждений компилятора (`-Wall -Wextra`), без зависимости от
наличия GPU, GGUF и активного окна.
