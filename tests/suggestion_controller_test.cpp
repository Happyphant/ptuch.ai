// suggestion_controller_test.cpp
#include <QtTest>

#include "suggestion/suggestion_controller.h"

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

QTEST_GUILESS_MAIN(SuggestionControllerTest)

#include "suggestion_controller_test.moc"
