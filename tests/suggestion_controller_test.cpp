// suggestion_controller_test.cpp
#include <QtTest>

#include "suggestion/suggestion_controller.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTextDocument>

// ---------------------------------------------------------------------------
// Тестовые дублёры: контроллер обязан работать БЕЗ виджетов —
// только через интерфейсы ISuggestionEditor / ITextGenerationBackend.
// ---------------------------------------------------------------------------

// In-memory замена QPlainTextEdit: QTextDocument + позиция курсора,
// сигналы-аналоги виджета (textChanged / cursorPositionChanged).
class FakeEditor final : public QObject, public ISuggestionEditor
{
    Q_OBJECT

public:
    // Эмуляция набора текста (курсор — в конец, как при печати).
    void typeText(const QString& text)
    {
        m_doc.setPlainText(text);
        m_cursor = text.size();
        emit textChanged();
        emit cursorPositionChanged();
    }

    // ISuggestionEditor
    QTextCursor textCursor() const override
    {
        QTextCursor cursor(&m_doc);
        cursor.setPosition(qBound(0, m_cursor,
                                 int(m_doc.characterCount()) - 1));
        return cursor;
    }
    QString documentText() const override { return m_doc.toPlainText(); }
    quint64 documentRevision() const override { return m_doc.revision(); }
    void insertText(const QString& text) override
    {
        QTextCursor cursor = textCursor();
        cursor.insertText(text);
        m_cursor = cursor.position();
        emit textChanged();
        emit cursorPositionChanged();
    }
    bool hasFocus() const override { return m_focused; }
    bool isReadOnly() const override { return false; }

    void setFocus(bool focused) { m_focused = focused; }

signals:
    void textChanged();
    void cursorPositionChanged();

private:
    mutable QTextDocument m_doc;
    int m_cursor = 0;
    bool m_focused = true;
};

// Backend-«руками»: записывает запросы/отмены и отвечает по требованию
// теста. cancel() НЕ подавляет ответ — это имитация backend'а,
// игнорирующего отмену: актуальность определяется только requestId.
// Тест и контроллер в одном потоке => сигналы доставляются синхронно.
class FakeBackend final : public ITextGenerationBackend
{
public:
    void generate(const GenerationRequest& request) override
    {
        requests.push_back(request);
    }

    void cancel(quint64 requestId) override
    {
        cancelled.push_back(requestId);
    }

    BackendDiagnostics diagnostics() const override
    {
        // Пусто по умолчанию; адаптер-тесты заполняют имена/модель.
        BackendDiagnostics diag;
        diag.backendName = backendName;
        diag.modelName = modelName;
        return diag;
    }

    // Возможность адаптеров задаётся тестом (по умолчанию — как у
    // текущих backend'ов: адаптеров нет).
    bool supportsAdapters() const override { return adapterSupport; }

    int count() const { return int(requests.size()); }

    void respond(int index, const QString& text)
    {
        GenerationResult result;
        result.requestId = requests[index].requestId;
        result.generatedText = text;
        result.stopReason = QStringLiteral("stop");
        result.elapsedMs = 1;
        emit generationReady(result);
    }

    void respondError(int index, const QString& message)
    {
        emit generationError(requests[index].requestId, message);
    }

    QVector<GenerationRequest> requests;
    QVector<quint64> cancelled;
    bool adapterSupport = false;
    QString backendName;
    QString modelName;
};

// ---------------------------------------------------------------------------
// Тесты SuggestionController
// ---------------------------------------------------------------------------
class SuggestionControllerTest final : public QObject
{
    Q_OBJECT

    using State = SuggestionController::State;

private slots:
    void init();
    void cleanup();

    void debounceSingleRequestWhileTyping(); // debounce + «печать продолжается»
    void noGenerationForEmptyContext();      // пустой контекст -> 0 запросов
    void manualRequestIsImmediate();         // requestSuggestion минует debounce
    void generationIdIncreasesPerRequest();
    void staleResponseIgnored();             // устаревший ответ отброшен
    void rejectCancelsPendingRequest();
    void backendErrorEmitsSuggestionFailed();
    void acceptSuggestionInsertsText();
    void styleMixClearsSuggestion();
    // Адаптеры (LoRA): явная ошибка до отправки при backend без
    // поддержки adapterPath; проверка совместимости с базовой моделью
    // и заполнение AdapterSpec — при поддержке.
    void adapterRejectedWhenBackendUnsupported();
    void adapterIncompatibleBaseModelRejected();
    void adapterAttachedWhenBackendSupports();
    void debounceIntervalApplies(); // setDebounceInterval: кламп + тайминг
    void autoSuggestionsGateKeepsManualAndActive();
    void settingsUpdateDuringGenerationKeepsRequest();

    // Гонки «новый запрос / отмена / приход старого ответа».
    void raceOfNewRequestCancelAndOldResponse();
    void staleErrorDroppedWhileNewRequestActive();
    // Смена фокуса: подсказка скрыта, debounce и запрос отменены.
    void focusLostHidesSuggestionAndCancels();
    // Закрытие приложения во время генерации (closeEvent -> shutdown):
    // cancel + отвязка backend, поздний ответ отброшен.
    void shutdownDuringGenerationDropsLateAnswer();
    // Логирование переходов состояний (категория ptuch.suggestion).
    void stateTransitionsAreLogged();

private:
    bool waitForRequests(int n)
    {
        return QTest::qWaitFor([this, n]() { return backend->count() >= n; },
                               3000);
    }

    FakeEditor* editor = nullptr;
    FakeBackend* backend = nullptr;
    SuggestionController* controller = nullptr;

    QSignalSpy* startedSpy = nullptr;
    QSignalSpy* readySpy = nullptr;
    QSignalSpy* failedSpy = nullptr;
    QSignalSpy* clearedSpy = nullptr;
    QSignalSpy* stateSpy = nullptr;
};

void SuggestionControllerTest::init()
{
    editor = new FakeEditor;
    backend = new FakeBackend;
    controller = new SuggestionController(editor);
    controller->setBackend(backend);

    // Как в MainWindow: сигналы редактора -> слоты контроллера.
    connect(editor, &FakeEditor::textChanged,
            controller, &SuggestionController::onTextChanged);
    connect(editor, &FakeEditor::cursorPositionChanged,
            controller, &SuggestionController::onCursorPositionChanged);

    startedSpy = new QSignalSpy(
        controller, &SuggestionController::suggestionStarted);
    readySpy = new QSignalSpy(
        controller, &SuggestionController::suggestionReady);
    failedSpy = new QSignalSpy(
        controller, &SuggestionController::suggestionFailed);
    clearedSpy = new QSignalSpy(
        controller, &SuggestionController::suggestionCleared);
    stateSpy = new QSignalSpy(
        controller, &SuggestionController::stateChanged);
}

void SuggestionControllerTest::cleanup()
{
    delete controller;
    delete startedSpy;
    delete readySpy;
    delete failedSpy;
    delete clearedSpy;
    delete stateSpy;
    delete backend;
    delete editor;
}

// Печать без паузы >= 500 мс не должна порождать ни одного запроса;
// после паузы — ровно один.
void SuggestionControllerTest::debounceSingleRequestWhileTyping()
{
    // Непрерывная печать 6 * 100 мс = 600 мс (> 500 мс debounce):
    // если бы таймер не перезапускался, запрос уже ушёл бы.
    for (int i = 1; i <= 6; ++i) {
        editor->typeText(QString(i, QLatin1Char('a')));
        QTest::qWait(100);
    }

    QCOMPARE(backend->count(), 0);
    QCOMPARE(controller->state(), State::Debouncing);

    // Печать остановилась -> ровно один запрос после debounce.
    QVERIFY(waitForRequests(1));
    QCOMPARE(controller->state(), State::Generating);
    QCOMPARE(startedSpy->count(), 1);

    // Повторного/лишнего запроса не появляется.
    QTest::qWait(700);
    QCOMPARE(backend->count(), 1);
    QCOMPARE(startedSpy->count(), 1);
}

void SuggestionControllerTest::noGenerationForEmptyContext()
{
    // Пустой документ: курсор в позиции 0 -> таймер даже не запускается.
    editor->typeText(QString());
    QTest::qWait(700);
    QCOMPARE(backend->count(), 0);
    QCOMPARE(controller->state(), State::Idle);

    // Только пробелы перед курсором -> запрос диспетчеризуется, но
    // пустой (trimmed) контекст не уходит в backend.
    editor->typeText(QStringLiteral("   "));
    QTest::qWait(700);
    QCOMPARE(backend->count(), 0);
    QCOMPARE(controller->state(), State::Idle);
    QCOMPARE(startedSpy->count(), 0);

    // Ручной запуск при пустом контексте -> видимая ошибка, без запроса.
    editor->typeText(QString());
    controller->requestSuggestion();
    QCOMPARE(backend->count(), 0);
    QCOMPARE(failedSpy->count(), 1);
    QCOMPARE(controller->state(), State::Error);
}

void SuggestionControllerTest::manualRequestIsImmediate()
{
    controller->setGenerationParams(QStringLiteral("You are a helper"),
                                    32, 0.2, 0.5);
    editor->typeText(QStringLiteral("hello"));

    // Никакого ожидания debounce: запрос уходит синхронно.
    controller->requestSuggestion();

    QCOMPARE(backend->count(), 1);
    QCOMPARE(startedSpy->count(), 1);
    QCOMPARE(controller->state(), State::Generating);

    // Запрос несёт явный requestId и все параметры генерации.
    const GenerationRequest& sent = backend->requests[0];
    QCOMPARE(sent.requestId, controller->generationId());
    QCOMPARE(sent.context, QStringLiteral("hello"));
    QCOMPARE(sent.systemPrompt, QStringLiteral("You are a helper"));
    QCOMPARE(sent.maxTokens, 32);
    QCOMPARE(sent.temperature, 0.2);
    QCOMPARE(sent.topP, 0.5);

    // Дебаунс-таймер не должен породить второй запрос.
    QTest::qWait(700);
    QCOMPARE(backend->count(), 1);
}

void SuggestionControllerTest::generationIdIncreasesPerRequest()
{
    editor->typeText(QStringLiteral("first"));
    QVERIFY(waitForRequests(1));
    const quint64 id1 = backend->requests[0].requestId;

    // Ответ принят -> состояние Ready, подсказка показана.
    backend->respond(0, QStringLiteral("first suggestion"));
    QCOMPARE(readySpy->count(), 1);
    QCOMPARE(controller->state(), State::Ready);

    // Второй запрос после новой печати.
    editor->typeText(QStringLiteral("first second"));
    QVERIFY(waitForRequests(2));
    const quint64 id2 = backend->requests[1].requestId;

    // Третий запрос: печать отменила предыдущий «в полёте».
    editor->typeText(QStringLiteral("first second third"));
    QVERIFY(waitForRequests(3));
    const quint64 id3 = backend->requests[2].requestId;

    QVERIFY(id1 > 0);
    QVERIFY(id2 > id1);
    QVERIFY(id3 > id2);
    QCOMPARE(controller->generationId(), id3);

    // Отмена «в полёте» ушла backend'у с прежним id.
    QVERIFY(backend->cancelled.contains(id2));
}

void SuggestionControllerTest::staleResponseIgnored()
{
    editor->typeText(QStringLiteral("first"));
    QVERIFY(waitForRequests(1));
    const quint64 id1 = backend->requests[0].requestId;

    // Пока первый запрос «в полёте», пользователь продолжает печатать:
    // debounce перезапускается и уходит второй запрос.
    editor->typeText(QStringLiteral("first second"));
    QVERIFY(waitForRequests(2));
    const quint64 id2 = backend->requests[1].requestId;
    QVERIFY(id2 > id1);

    // Ответ на ПЕРВЫЙ (устаревший) запрос приходит вторым — игнорируем,
    // даже что FakeBackend не подчинился cancel().
    backend->respond(0, QStringLiteral("stale suggestion"));
    QCOMPARE(readySpy->count(), 0);
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(controller->state() == State::Debouncing ||
            controller->state() == State::Generating);

    // Ответ на актуальный запрос принимается с его generation id.
    backend->respond(1, QStringLiteral("fresh suggestion"));
    QCOMPARE(readySpy->count(), 1);
    QCOMPARE(readySpy->at(0).at(0).toString(),
             QStringLiteral("fresh suggestion"));
    QCOMPARE(readySpy->at(0).at(1).toULongLong(), id2);
    QCOMPARE(controller->state(), State::Ready);
    QVERIFY(controller->hasSuggestion());
}

void SuggestionControllerTest::rejectCancelsPendingRequest()
{
    editor->typeText(QStringLiteral("hello"));
    QVERIFY(waitForRequests(1));
    const quint64 id = backend->requests[0].requestId;

    controller->rejectSuggestion();

    QCOMPARE(controller->state(), State::Idle);
    QVERIFY(!controller->hasSuggestion());
    QCOMPARE(clearedSpy->count(), 0); // показанной подсказки не было
    // Отмена дошла до backend'а с тем самым requestId.
    QVERIFY(backend->cancelled.contains(id));

    // Ответ отменённого запроса не «воскрешает» ни подсказку, ни состояние.
    backend->respond(0, QStringLiteral("zombie suggestion"));
    QCOMPARE(readySpy->count(), 0);
    QVERIFY(!controller->hasSuggestion());
    QCOMPARE(controller->state(), State::Idle);

    // И debounce не перезапущен.
    QTest::qWait(700);
    QCOMPARE(backend->count(), 1);
}

void SuggestionControllerTest::backendErrorEmitsSuggestionFailed()
{
    editor->typeText(QStringLiteral("hello"));
    QVERIFY(waitForRequests(1));
    const quint64 id = backend->requests[0].requestId;

    backend->respondError(0, QStringLiteral("model load failed"));

    QCOMPARE(failedSpy->count(), 1);
    QCOMPARE(failedSpy->at(0).at(0).toString(),
             QStringLiteral("model load failed"));
    QCOMPARE(failedSpy->at(0).at(1).toULongLong(), id);
    QCOMPARE(controller->state(), State::Error);
    QVERIFY(!controller->hasSuggestion());
}

void SuggestionControllerTest::acceptSuggestionInsertsText()
{
    editor->typeText(QStringLiteral("hello"));
    QVERIFY(waitForRequests(1));

    backend->respond(0, QStringLiteral(" world"));
    QVERIFY(controller->hasSuggestion());
    QCOMPARE(controller->suggestion(), QStringLiteral(" world"));

    controller->acceptSuggestion();

    // Подсказка вставлена в документ и очищена из состояния.
    QCOMPARE(editor->documentText(), QStringLiteral("hello world"));
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(clearedSpy->count() >= 1);

    // Вставка спровоцировала textChanged -> новый debounce-запрос,
    // подсказка принимается только один раз.
    QVERIFY(waitForRequests(2));
    QCOMPARE(startedSpy->count(), 2);
}

void SuggestionControllerTest::styleMixClearsSuggestion()
{
    editor->typeText(QStringLiteral("hello"));
    QVERIFY(waitForRequests(1));

    backend->respond(0, QStringLiteral(" world"));
    QVERIFY(controller->hasSuggestion());

    controller->setStyleMix(
        QVector<StyleWeight>{StyleWeight{QStringLiteral("poetic"), 0.5f}});

    // Старая подсказка сделана под прежний mix — она убрана.
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(clearedSpy->count() >= 1);

    // И смешанный стиль попал в новый запрос.
    QVERIFY(waitForRequests(2));
    QCOMPARE(backend->requests[1].styleWeights.size(), 1);
    QCOMPARE(backend->requests[1].styleWeights.first().styleId,
             QStringLiteral("poetic"));
}

// Адаптер активного стиля + backend без поддержки (supportsAdapters()
// == false): ЯВНАЯ ошибка до отправки — 0 запросов, состояние Error.
// Никакого тихого игнора и никакой фиктивной загрузки LoRA.
void SuggestionControllerTest::adapterRejectedWhenBackendUnsupported()
{
    QVERIFY(!backend->supportsAdapters()); // значение по умолчанию
    backend->backendName = QStringLiteral("fake");

    editor->typeText(QStringLiteral("hello"));

    StyleProfile profile;
    profile.id = QStringLiteral("loraStyle");
    profile.displayName = QStringLiteral("Стиль с LoRA");
    profile.adapterPath = QStringLiteral("/models/style-lora.safetensors");
    controller->setStyleProfiles(QVector<StyleProfile>{profile});
    controller->setStyleMix(
        QVector<StyleWeight>{StyleWeight{QStringLiteral("loraStyle"), 1.0f}});

    controller->requestSuggestion();

    QCOMPARE(backend->count(), 0);
    QCOMPARE(failedSpy->count(), 1);
    const QString message = failedSpy->last().at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("не поддерживает adapterPath")),
             qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("style-lora.safetensors")),
             qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("fake")), qPrintable(message));
    QCOMPARE(controller->state(), State::Error);
}

// Backend адаптеры поддерживает, но адаптер обучен под другую модель:
// явная ошибка несовместимости до отправки (0 запросов) с обоими
// идентификаторами в тексте.
void SuggestionControllerTest::adapterIncompatibleBaseModelRejected()
{
    backend->adapterSupport = true;
    backend->backendName = QStringLiteral("fake");
    backend->modelName = QStringLiteral("other-7b");

    editor->typeText(QStringLiteral("hello"));

    StyleProfile profile;
    profile.id = QStringLiteral("loraStyle");
    profile.displayName = QStringLiteral("Стиль с LoRA");
    profile.adapterPath = QStringLiteral("/models/style-lora.safetensors");
    profile.baseModelId = QStringLiteral("qwen2.5-3b");
    controller->setStyleProfiles(QVector<StyleProfile>{profile});
    controller->setStyleMix(
        QVector<StyleWeight>{StyleWeight{QStringLiteral("loraStyle"), 1.0f}});

    controller->requestSuggestion();

    QCOMPARE(backend->count(), 0);
    QCOMPARE(failedSpy->count(), 1);
    const QString message = failedSpy->last().at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("несовместимость")),
             qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("qwen2.5-3b")),
             qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("other-7b")),
             qPrintable(message));
    QCOMPARE(controller->state(), State::Error);
}

// Backend поддерживает адаптеры и базовая модель совпала (без учёта
// регистра): запрос уходит с заполненным нейтральным AdapterSpec —
// загрузка остаётся работой backend'а, контроллер лишь передаёт данные.
void SuggestionControllerTest::adapterAttachedWhenBackendSupports()
{
    backend->adapterSupport = true;
    backend->backendName = QStringLiteral("fake");
    backend->modelName = QStringLiteral("Qwen2.5-3B");

    editor->typeText(QStringLiteral("hello"));

    StyleProfile profile;
    profile.id = QStringLiteral("loraStyle");
    profile.displayName = QStringLiteral("Стиль с LoRA");
    profile.adapterPath = QStringLiteral("/models/style-lora.safetensors");
    profile.adapterType = QStringLiteral("lora");
    profile.baseModelId = QStringLiteral("qwen2.5-3b");
    profile.promptTag = QStringLiteral("<|style|>");
    profile.adapterScale = 0.7;
    controller->setStyleProfiles(QVector<StyleProfile>{profile});
    controller->setStyleMix(
        QVector<StyleWeight>{StyleWeight{QStringLiteral("loraStyle"), 1.0f}});

    controller->requestSuggestion();
    QVERIFY(waitForRequests(1));

    QCOMPARE(startedSpy->count(), 1);
    const GenerationRequest& request = backend->requests.first();
    QCOMPARE(request.adapter.path,
             QStringLiteral("/models/style-lora.safetensors"));
    QCOMPARE(request.adapter.type, QStringLiteral("lora"));
    QCOMPARE(request.adapter.baseModelId, QStringLiteral("qwen2.5-3b"));
    QCOMPARE(request.adapter.promptTag, QStringLiteral("<|style|>"));
    QCOMPARE(request.adapter.scale, 0.7);
}

// setDebounceInterval: значение применяется к следующему запуску
// таймера (запущенный не перезапускается) и клампится границами.
void SuggestionControllerTest::debounceIntervalApplies()
{
    QCOMPARE(controller->debounceInterval(), 500);

    controller->setDebounceInterval(50);
    QCOMPARE(controller->debounceInterval(), 50);

    // Сеттер санирует сам: за границами [0; 60000] не уходит.
    controller->setDebounceInterval(-10);
    QCOMPARE(controller->debounceInterval(), 0);
    controller->setDebounceInterval(999999);
    QCOMPARE(controller->debounceInterval(), 60000);

    controller->setDebounceInterval(50);

    // С новым интервалом запрос уходит заметно раньше старых 500 мс.
    QElapsedTimer timer;
    timer.start();
    editor->typeText(QStringLiteral("hello"));
    QVERIFY(waitForRequests(1));
    QVERIFY2(timer.elapsed() < 450,
             "Debounce должен использоваться новый (50 мс), а не 500");
}

// Выключение автоматических подсказок гасит debounce-путь, но НЕ
// ручной запрос и НЕ активную генерацию.
void SuggestionControllerTest::autoSuggestionsGateKeepsManualAndActive()
{
    // Хвост debounce «в полёте» гаснет при выключении авто.
    editor->typeText(QStringLiteral("hello"));
    QCOMPARE(controller->state(), State::Debouncing);

    controller->setAutoSuggestions(false);
    QCOMPARE(controller->state(), State::Idle);
    QTest::qWait(700);
    QCOMPARE(backend->count(), 0);

    // Печать при выключенном авто не порождает запросов...
    editor->typeText(QStringLiteral("hello world"));
    QTest::qWait(700);
    QCOMPARE(backend->count(), 0);
    QCOMPARE(controller->state(), State::Idle);

    // ...но ручной запрос работает.
    controller->requestSuggestion();
    QCOMPARE(backend->count(), 1);
    QCOMPARE(controller->state(), State::Generating);

    // Включение авто возвращает debounce-путь.
    controller->setAutoSuggestions(true);
    QVERIFY(controller->autoSuggestions());
    editor->typeText(QStringLiteral("hello world!"));
    QVERIFY(waitForRequests(2));
}

// Обновление настроек посреди активной генерации: запрос не отменяется
// и не перезапускается, завершается со своим id; новые параметры
// применяются к СЛЕДУЮЩЕМУ запросу.
void SuggestionControllerTest::settingsUpdateDuringGenerationKeepsRequest()
{
    editor->typeText(QStringLiteral("hello"));
    QVERIFY(waitForRequests(1));
    const quint64 id = backend->requests[0].requestId;
    QCOMPARE(controller->state(), State::Generating);

    controller->setDebounceInterval(750);
    controller->setGenerationParams(QString(), 128, 0.9, 0.95);
    controller->setAutoSuggestions(false);

    // Ни отмены, ни нового запроса, ни смены состояния.
    QCOMPARE(backend->count(), 1);
    QVERIFY(backend->cancelled.isEmpty());
    QCOMPARE(controller->state(), State::Generating);

    // Активная генерация доходит до конца со СВОИМ generation id.
    backend->respond(0, QStringLiteral("alive"));
    QCOMPARE(readySpy->count(), 1);
    QCOMPARE(readySpy->at(0).at(1).toULongLong(), id);
    QCOMPARE(controller->state(), State::Ready);
    QCOMPARE(controller->suggestion(), QStringLiteral("alive"));

    // Следующий запрос (ручной — авто выключены) несёт НОВЫЕ параметры.
    controller->requestSuggestion();
    QCOMPARE(backend->count(), 2);
    const GenerationRequest& second = backend->requests[1];
    QCOMPARE(second.maxTokens, 128);
    QCOMPARE(second.temperature, 0.9);
    QCOMPARE(second.topP, 0.95);
}

namespace {

// --- Перехват лога контроллера (категория ptuch.suggestion) -----------
QStringList g_suggestionLog;
QtMessageHandler g_previousMessageHandler = nullptr;

void suggestionLogHandler(QtMsgType type, const QMessageLogContext& context,
                          const QString& message)
{
    if (type == QtInfoMsg &&
        qstrcmp(context.category, "ptuch.suggestion") == 0)
        g_suggestionLog.append(message);

    // Вывод не глушим: сообщение уходит и прежнему handler'у (QTest).
    if (g_previousMessageHandler != nullptr)
        g_previousMessageHandler(type, context, message);
}

bool logMatches(const QRegularExpression& pattern)
{
    for (const QString& line : g_suggestionLog) {
        if (pattern.match(line).hasMatch())
            return true;
    }
    return false;
}

// RAII: включает категорию, ставит перехват, восстанавливает оба
// даже при падении QVERIFY внутри теста (деструктор — не ранний return).
class SuggestionLogCapture
{
public:
    SuggestionLogCapture()
    {
        g_suggestionLog.clear();
        // Явное включение: не зависим от дефолтных правил логирования.
        QLoggingCategory::setFilterRules(
            QStringLiteral("ptuch.suggestion.info=true"));
        g_previousMessageHandler =
            qInstallMessageHandler(suggestionLogHandler);
    }
    ~SuggestionLogCapture()
    {
        qInstallMessageHandler(g_previousMessageHandler);
        g_previousMessageHandler = nullptr;
        QLoggingCategory::setFilterRules(QString());
    }

    SuggestionLogCapture(const SuggestionLogCapture&) = delete;
    SuggestionLogCapture& operator=(const SuggestionLogCapture&) = delete;
};

} // namespace

// Гонка «новый запрос vs отмена vs приход старого ответа»: отменённый
// запрос (backend проигнорировал cancel) отвечает ПОСЛЕ старта нового —
// ответ отброшен, состояние нового запроса не тронуто (остаётся
// Generating), свежий ответ принимается со своим id.
void SuggestionControllerTest::raceOfNewRequestCancelAndOldResponse()
{
    SuggestionLogCapture logCapture;

    // (1) Первый запрос «в полёте».
    editor->typeText(QStringLiteral("first"));
    QVERIFY(waitForRequests(1));
    const quint64 id1 = backend->requests[0].requestId;

    // (2) Отмена (Escape-путь): cancel ушёл backend'у, состояние Idle.
    controller->rejectSuggestion();
    QVERIFY(backend->cancelled.contains(id1));
    QCOMPARE(controller->state(), State::Idle);

    // (3) Новый запрос стартует ДО прихода старого ответа.
    controller->requestSuggestion();
    QCOMPARE(backend->count(), 2);
    const quint64 id2 = backend->requests[1].requestId;
    QVERIFY(id2 > id1);
    QCOMPARE(controller->generationId(), id2);
    QCOMPARE(controller->state(), State::Generating);

    // (4) Старый ответ приходит ПОСЛЕ нового запроса: не заменяет
    //     новый и не сбрасывает его состояние.
    backend->respond(0, QStringLiteral("старый ответ"));
    QCOMPARE(readySpy->count(), 0);
    QVERIFY(!controller->hasSuggestion());
    QCOMPARE(controller->state(), State::Generating);
    // Обе гонки видны в логе: отмена и отброшенный stale-ответ.
    QVERIFY(logMatches(QRegularExpression(
        QStringLiteral("cancel: активный запрос id=%1 отменён").arg(id1))));
    QVERIFY(logMatches(QRegularExpression(
        QStringLiteral("stale: ответ id=%1 отброшен").arg(id1))));

    // (5) Ответ нового запроса принимается с его id.
    backend->respond(1, QStringLiteral("свежий ответ"));
    QCOMPARE(readySpy->count(), 1);
    QCOMPARE(readySpy->at(0).at(0).toString(),
             QStringLiteral("свежий ответ"));
    QCOMPARE(readySpy->at(0).at(1).toULongLong(), id2);
    QCOMPARE(controller->state(), State::Ready);
}

// Ошибка устаревшего запроса не трогает текущий: пока «в полёте» новый
// запрос, старая ошибка не испускает suggestionFailed и не уводит
// состояние в Error.
void SuggestionControllerTest::staleErrorDroppedWhileNewRequestActive()
{
    editor->typeText(QStringLiteral("first"));
    QVERIFY(waitForRequests(1));

    editor->typeText(QStringLiteral("first second"));
    QVERIFY(waitForRequests(2));
    QCOMPARE(controller->state(), State::Generating);

    // Ошибка на ПЕРВЫЙ (отменённый) запрос — игнорируется.
    backend->respondError(0, QStringLiteral("stale failure"));
    QCOMPARE(failedSpy->count(), 0);
    QCOMPARE(controller->state(), State::Generating);

    // Ошибка на АКТУАЛЬНЫЙ запрос доходит штатно.
    backend->respondError(1, QStringLiteral("real failure"));
    QCOMPARE(failedSpy->count(), 1);
    QCOMPARE(failedSpy->at(0).at(0).toString(),
             QStringLiteral("real failure"));
    QCOMPARE(controller->state(), State::Error);
}

// Потеря фокуса: показанная подсказка скрывается, debounce гаснет,
// запрос «в полёте» отменяется — поздний ответ не воскресает подсказку
// и не уводит состояние в Error из-за того, что фокус ушёл.
void SuggestionControllerTest::focusLostHidesSuggestionAndCancels()
{
    // Показанная подсказка гаснет при потере фокуса.
    editor->typeText(QStringLiteral("hello"));
    QVERIFY(waitForRequests(1));
    backend->respond(0, QStringLiteral("hint"));
    QCOMPARE(controller->state(), State::Ready);
    QVERIFY(controller->hasSuggestion());

    editor->setFocus(false);
    controller->onFocusLost();

    QVERIFY(!controller->hasSuggestion());
    QCOMPARE(clearedSpy->count(), 1);
    QCOMPARE(controller->state(), State::Idle);

    // Debounce «в полёте» не воскресает запрос после потери фокуса.
    QTest::qWait(700);
    QCOMPARE(backend->count(), 1);
    QCOMPARE(controller->state(), State::Idle);

    // Запрос «в полёте» отменяется; поздний ответ отбрасывается.
    editor->setFocus(true);
    controller->requestSuggestion();
    QCOMPARE(backend->count(), 2);
    const quint64 id2 = backend->requests[1].requestId;
    QCOMPARE(controller->state(), State::Generating);

    editor->setFocus(false);
    controller->onFocusLost();
    QVERIFY(backend->cancelled.contains(id2));
    QCOMPARE(controller->state(), State::Idle);

    backend->respond(1, QStringLiteral("late"));
    QCOMPARE(readySpy->count(), 1);          // только «hint», не «late»
    QVERIFY(!controller->hasSuggestion());
    QCOMPARE(controller->state(), State::Idle); // не Error

    // Возврат фокуса + печать -> debounce; новая потеря фокуса гасит.
    editor->setFocus(true);
    editor->typeText(QStringLiteral("hello world"));
    QCOMPARE(controller->state(), State::Debouncing);
    editor->setFocus(false);
    controller->onFocusLost();
    QCOMPARE(controller->state(), State::Idle);
    QTest::qWait(700);
    QCOMPARE(backend->count(), 2);
}

// Закрытие приложения во время активной генерации — unit-уровень того,
// что MainWindow::closeEvent делает через shutdown(): debounce остановлен,
// активный запрос отменён, backend отвязан. Поздний ответ «мёртвого»
// backend'а отбрасывается, повторный запуск невозможен (enabled = false).
void SuggestionControllerTest::shutdownDuringGenerationDropsLateAnswer()
{
    editor->typeText(QStringLiteral("hello"));
    controller->requestSuggestion(); // ручной: без debounce и без фокуса
    QCOMPARE(controller->state(), State::Generating);
    QCOMPARE(backend->count(), 1);

    const quint64 inFlightId = backend->requests[0].requestId;
    controller->shutdown();

    // Активный запрос отменён (cancel ушёл ровно для него), контроллер
    // отвязан от backend'а и выключен.
    const QVector<quint64> expectedCancelled{inFlightId};
    QCOMPARE(backend->cancelled, expectedCancelled);
    QVERIFY(controller->backend() == nullptr);
    QVERIFY(!controller->isEnabled());
    QCOMPARE(controller->state(), State::Idle);

    // Поздний ответ отвязанного backend'а: ни ready, ни подсказки,
    // состояние не меняется.
    backend->respond(0, QStringLiteral("поздний ответ"));
    QCOMPARE(readySpy->count(), 0);
    QVERIFY(!controller->hasSuggestion());
    QCOMPARE(controller->state(), State::Idle);

    // Ручной запуск после shutdown не проходит: запрос не уходит
    // (backend отвязан), ошибка видима — контроллер мёртв.
    controller->requestSuggestion();
    QCOMPARE(backend->count(), 1);
    QCOMPARE(failedSpy->count(), 1);
    QCOMPARE(controller->state(), State::Error);
}

// Каждый сдвиг машины состояний логируется в категорию ptuch.suggestion
// строкой вида «state: <From> -> <To> (id=N)» — по ней виден весь
// сценарий печати/запроса/готовности и гонки (cancel/stale рядом).
void SuggestionControllerTest::stateTransitionsAreLogged()
{
    SuggestionLogCapture logCapture;

    editor->typeText(QStringLiteral("hello"));       // Idle -> Debouncing
    QVERIFY(waitForRequests(1));                     // Debouncing -> Generating
    backend->respond(0, QStringLiteral("hint"));     // Generating -> Ready
    editor->typeText(QStringLiteral("hello world")); // Ready -> Debouncing
    controller->rejectSuggestion();                  // Debouncing -> Idle

    QVERIFY(logMatches(QRegularExpression(
        QStringLiteral("^state: Idle -> Debouncing \\(id=\\d+\\)"))));
    QVERIFY(logMatches(QRegularExpression(
        QStringLiteral("state: Debouncing -> Generating"))));
    QVERIFY(logMatches(QRegularExpression(
        QStringLiteral("state: Generating -> Ready"))));
    QVERIFY(logMatches(QRegularExpression(
        QStringLiteral("state: Ready -> Debouncing"))));
    QVERIFY(logMatches(QRegularExpression(
        QStringLiteral("state: Debouncing -> Idle"))));

    // Повторный запуск не плодит дублей: setState фильтрует
    // неизменившиеся состояния (ровно по строке на переход).
    const int stateLines = [&]() {
        int n = 0;
        const QRegularExpression pattern(
            QStringLiteral("^state: \\w+ -> \\w+"));
        for (const QString& line : g_suggestionLog) {
            if (pattern.match(line).hasMatch())
                ++n;
        }
        return n;
    }();
    QCOMPARE(stateLines, 5);
}

QTEST_GUILESS_MAIN(SuggestionControllerTest)

#include "suggestion_controller_test.moc"
