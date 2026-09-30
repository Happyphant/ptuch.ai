// performance_test.cpp
// Замеры производительности ПТЮЧ.AI для PERFORMANCE_REVIEW.md.
// Методика:
//  - только QElapsedTimer/QTimer и условия qWaitFor — «спим» ровно
//    столько, сколько нужно доказать отсутствие события;
//  - always-on слоты НЕ требуют фокуса окна, реальной модели и GPU:
//    debounce меряется на контроллерном дублёре (логика таймера от
//    OS-фокуса не зависит), ручной requestSuggestion фокуса не требует;
//  - слот с реальной моделью закрыт гейтом PTUCH_MODEL_TESTS=1
//    (запуск из каталога проекта — models/ рядом);
//  - каждая метрика печатается строкой «METRIC ...» — по ним
//    собирается отчёт.
#include <QtTest>

#include "UI/MainWindow.h"
#include "UI/suggestion_overlay.h"
#include "backend/text_generation_backend.h"
#include "llama/llama_backend.h"
#include "suggestion/document_state.h"
#include "suggestion/editor_adapter.h"
#include "suggestion/prompt_builder.h"
#include "suggestion/style_profile.h"
#include "suggestion/suggestion_controller.h"

#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTimer>

#include <functional>

#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#elif defined(Q_OS_LINUX)
#include <QFile>
#include <unistd.h>
#endif

namespace {

// Текущее RSS процесса (байты); -1 — недоступно на этой платформе.
// По нему меряется память: базовый уровень Qt, модель, накопление
// по циклам запросов.
qint64 rssBytes()
{
#if defined(Q_OS_MACOS)
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count)
        != KERN_SUCCESS) {
        return -1;
    }
    return static_cast<qint64>(info.resident_size);
#elif defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/self/statm"));
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> parts =
        file.readAll().simplified().split(' ');
    if (parts.size() < 2)
        return -1;
    return parts.at(1).toLongLong()
        * static_cast<qint64>(sysconf(_SC_PAGESIZE));
#else
    return -1;
#endif
}

// Контроль блокировок UI-потока: тик PreciseTimer 1 мс; разрыв между
// тиками — время, пока главный цикл событий не обслуживал таймер
// (длинная синхронная операция в UI-потоке). Пока идёт чистое
// ожидание (processEvents), разрывы ≈ 1–2 мс.
class StallWatchdog
{
public:
    void start()
    {
        m_clock.start();
        m_lastMs = 0;
        m_maxGapMs = 0;
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(1);
        QObject::connect(&m_timer, &QTimer::timeout, [this]() {
            const qint64 now = m_clock.elapsed();
            if (m_lastMs > 0)
                m_maxGapMs = qMax(m_maxGapMs, now - m_lastMs);
            m_lastMs = now;
        });
        m_timer.start();
    }

    void stop() { m_timer.stop(); }

    qint64 maxGapMs() const { return m_maxGapMs; }

private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastMs = 0;
    qint64 m_maxGapMs = 0;
};

// Синхронный recording-backend (живёт в потоке теста): фиксирует
// запросы и отмены, отвечает по требованию либо через короткий
// QTimer — детерминизм без потоков.
class RecordingBackend final : public ITextGenerationBackend
{
    Q_OBJECT

public:
    QVector<GenerationRequest> requests;
    QVector<quint64> cancelledIds;

    void generate(const GenerationRequest& request) override
    {
        requests.append(request);
        if (m_autoRespondMs >= 0) {
            QTimer::singleShot(m_autoRespondMs, this,
                               [this, id = request.requestId]() {
                                   const GenerationResult result{
                                       id, m_autoText,
                                       QStringLiteral("stop"), 0};
                                   emit generationReady(result);
                               });
        }
    }

    void cancel(quint64 requestId) override
    {
        cancelledIds.append(requestId);
    }

    BackendDiagnostics diagnostics() const override { return {}; }

    void setAutoRespond(int delayMs, const QString& text)
    {
        m_autoRespondMs = delayMs;
        m_autoText = text;
    }

private:
    int m_autoRespondMs = -1;
    QString m_autoText = QStringLiteral(" продолжение");
};

// Контроллерный дубль редактора (как в SuggestionControllerTests):
// in-memory документ; hasFocus() всегда true — чтобы debounce-путь
// был активен без OS-фокуса (тикает логика таймера, а не фокус ОС).
class PerfFakeEditor final : public QObject, public ISuggestionEditor
{
    Q_OBJECT

public:
    void typeText(const QString& text)
    {
        m_doc.setPlainText(text);
        m_cursor = text.size();
        emit textChanged();
        emit cursorPositionChanged();
    }

    QTextCursor textCursor() const override
    {
        QTextCursor cursor(&m_doc);
        cursor.setPosition(qBound(0, m_cursor,
                                  int(m_doc.characterCount()) - 1));
        return cursor;
    }
    QString documentText() const override { return m_doc.toPlainText(); }
    quint64 documentRevision() const override
    {
        return quint64(m_doc.revision());
    }
    void insertText(const QString& text) override
    {
        QTextCursor cursor = textCursor();
        cursor.insertText(text);
        m_cursor = cursor.position();
        emit textChanged();
        emit cursorPositionChanged();
    }
    bool hasFocus() const override { return true; }
    bool isReadOnly() const override { return false; }

signals:
    void textChanged();
    void cursorPositionChanged();

private:
    mutable QTextDocument m_doc;
    int m_cursor = 0;
};

QString repeatedToSize(const QString& unit, int minChars)
{
    QString text;
    text.reserve(minChars + unit.size());
    while (text.size() < minChars)
        text += unit;
    return text;
}

} // namespace

class PerformanceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // Всегда в ctest (без модели, без фокуса):
    void debounceStopToRequestStartLatency();
    void promptBuilderPreparationCost();
    void requestAssemblyCostOnLargeDocument();
    void requestPayloadStaysBounded();
    void ghostDisplayLatencyAndPaintCost();
    void uiThreadStaysResponsiveDuringScenario();
    void cancelLatencyAndStaleAnswerDrop();
    void mainWindowCyclesDoNotAccumulateObjectsOrMemory();

    // Гейт PTUCH_MODEL_TESTS=1 (реальная GGUF):
    void realModelPhasesThroughputMemoryAndCancel();

protected:
    // Ловит Paint-события overlay для метрики «готовность -> первый
    // кадр» в слоте ghostDisplayLatencyAndPaintCost.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool m_watchPaint = false;
    QElapsedTimer m_clock;
    qint64 m_tReadyUs = -1;
    qint64 m_tFirstPaintUs = -1;
    qint64 m_paintCount = 0;

    QByteArray m_previousDisableLlama;
    bool m_hadDisableLlama = false;
};

void PerformanceTest::initTestCase()
{
    // Always-on слоты с MainWindow не должны грузить llama: сохраняем
    // и выставляем PTUCH_DISABLE_LLAMA (слот реальной модели работает
    // с LlamaBackend напрямую — ему флаг не мешает).
    m_hadDisableLlama = qEnvironmentVariableIsSet("PTUCH_DISABLE_LLAMA");
    if (m_hadDisableLlama)
        m_previousDisableLlama = qgetenv("PTUCH_DISABLE_LLAMA");
    qputenv("PTUCH_DISABLE_LLAMA", "1");
}

void PerformanceTest::cleanupTestCase()
{
    if (m_hadDisableLlama)
        qputenv("PTUCH_DISABLE_LLAMA", m_previousDisableLlama);
    else
        qunsetenv("PTUCH_DISABLE_LLAMA");
}

bool PerformanceTest::eventFilter(QObject* watched, QEvent* event)
{
    if (m_watchPaint && m_tReadyUs >= 0 && watched != nullptr
        && event->type() == QEvent::Paint
        && m_tFirstPaintUs < 0) {
        m_tFirstPaintUs = m_clock.nsecsElapsed() / 1000; // микросекунды
        ++m_paintCount;
    }
    return QObject::eventFilter(watched, event);
}

// --- Метрика 1: остановка печати -> старт запроса ----------------------
// Реальный путь контроллера (debounce-таймер + scheduleRequest),
// измеряется от ПОСЛЕДНЕЙ вставки символа до сигнала suggestionStarted.
void PerformanceTest::debounceStopToRequestStartLatency()
{
    PerfFakeEditor editor;
    SuggestionController controller(&editor);
    RecordingBackend backend;
    controller.setBackend(&backend);
    controller.setDebounceInterval(500);

    connect(&editor, &PerfFakeEditor::textChanged,
            &controller, &SuggestionController::onTextChanged);
    connect(&editor, &PerfFakeEditor::cursorPositionChanged,
            &controller, &SuggestionController::onCursorPositionChanged);

    QElapsedTimer clock;
    clock.start();
    qint64 tStartedMs = -1;
    connect(&controller, &SuggestionController::suggestionStarted,
            this, [&tStartedMs, &clock]() {
                tStartedMs = clock.elapsed(); // elapsed() — миллисекунды
            });

    // Печать: 10 вставок с интервалом ~30 мс — ни одна не должна
    // стартовать запрос (debounce перезапускается на каждой вставке).
    QString text;
    for (int i = 0; i < 10; ++i) {
        text += QStringLiteral("word%1 ").arg(i);
        editor.typeText(text);
        if (i + 1 < 10)
            QTest::qWait(30);
    }
    const qint64 tStopMs = clock.elapsed();

    QVERIFY(QTest::qWaitFor([&tStartedMs]() { return tStartedMs >= 0; },
                            5000));
    const qint64 latencyMs = tStartedMs - tStopMs;
    qInfo().noquote() << QStringLiteral(
        "METRIC debounce: последняя вставка -> suggestionStarted = %1 мс "
        "(интервал 500 мс)")
                             .arg(latencyMs);
    qInfo() << "METRIC-raw tStopMs" << tStopMs << "tStartedMs"
            << tStartedMs << "deltaMs" << (tStartedMs - tStopMs);

    // Ровно один запрос после остановки печати.
    QTest::qWait(80);
    QCOMPARE(backend.requests.size(), 1);
    // CoarseTimer: срабатывание 500 ±5% + джиттер планировщика.
    QVERIFY2(latencyMs >= 450 && latencyMs <= 700,
             qPrintable(QStringLiteral("вне ожидаемого окна: %1 мс")
                            .arg(latencyMs)));
}

// --- Метрика 2: подготовка prompt'а (PromptBuilder) --------------------
// Стоимость сборки system/user prompt. Сумма итераций >> шума
// таймера; нормировка — на один вызов.
void PerformanceTest::promptBuilderPreparationCost()
{
    const auto measure = [](const PromptInput& input, int iterations,
                            const char* label) {
        // Прогрев (первый вызов — аллокации).
        PromptBuilder::build(input);

        QElapsedTimer timer;
        timer.start();
        AutocompletePrompt prompt;
        for (int i = 0; i < iterations; ++i)
            prompt = PromptBuilder::build(input);
        const double avgUs =
            timer.nsecsElapsed() / double(iterations) / 1000.0;
        qInfo().noquote() << QStringLiteral(
            "METRIC promptBuilder %1: %2 мкс/вызов (%3 итераций, "
            "system=%4 симв., user=%5 симв.)")
                                 .arg(QString::fromLatin1(label))
                                 .arg(QString::number(avgUs, 'f', 2))
                                 .arg(iterations)
                                 .arg(prompt.systemPrompt.size())
                                 .arg(prompt.userPrompt.size());
        return avgUs;
    };

    // Типовой контекст ~200 символов + 4 активных стиля.
    PromptInput typical;
    typical.context = repeatedToSize(
        QStringLiteral("Съешь же ещё этих мягких французских булок, да выпей "
                       "же чаю. "),
        200);
    typical.styles = builtinStyleProfiles();
    for (StyleProfile& profile : typical.styles)
        profile.weight = 0.6;
    typical.language = QStringLiteral("ru");

    // Длинный контекст у верхней границы (2000 символов — cap
    // DocumentState) без стилей; язык автоопределён.
    PromptInput longRu;
    longRu.context = repeatedToSize(
        QStringLiteral("Очень длинный русский контекст документа для "
                       "автодополнения. "),
        2000);
    longRu.language = QStringLiteral("ru");

    const double typicalUs = measure(typical, 5000, "ru200+4styles");
    const double longUs = measure(longRu, 5000, "ru2000");

    QVERIFY2(typicalUs < 1000,
             "Сборка промпта не должна стоить миллисекунды");
    QVERIFY2(longUs < 1000,
             "Сборка промпта с длинным контекстом не должна стоить "
             "миллисекунды");
}

// --- Метрика 3: сборка запроса в UI-потоке (1 МБ документ) -------------
// Путь: documentText() (полная копия toPlainText) + снимок DocumentState
// + обрезка префикса + постановка в очередь backend'а. Всё это
// синхронно в UI-потоке — метрика = вклад в задержку UI.
void PerformanceTest::requestAssemblyCostOnLargeDocument()
{
    QPlainTextEdit editor;
    const QString big = repeatedToSize(
        QStringLiteral("Абзац текста редактора: строки, слова, кириллица "
                       "и пунктуация. "),
        1024 * 1024);
    editor.setPlainText(big);
    editor.moveCursor(QTextCursor::End);

    // (а) полная копия документа — что стоит documentText() на запрос.
    {
        QElapsedTimer timer;
        timer.start();
        qint64 charsTotal = 0;
        const int iterations = 50;
        for (int i = 0; i < iterations; ++i)
            charsTotal += editor.toPlainText().size();
        const qint64 avgUs = timer.nsecsElapsed() / 1000 / iterations;
        qInfo().noquote() << QStringLiteral(
            "METRIC documentText(toPlainText, 1 МБ): %1 мкс/вызов "
            "(%2 симв.)")
                                 .arg(avgUs)
                                 .arg(charsTotal / iterations);
    }

    // (б) снимок документа + сборка контекста (обрезка префикса/суффикса).
    {
        QElapsedTimer timer;
        timer.start();
        const int iterations = 200;
        for (int i = 0; i < iterations; ++i) {
            const DocumentState state = DocumentState::capture(
                big, big.size(), QString(), /*generationId*/ 7,
                /*lastModifiedMs*/ 0);
            const DocumentContext ctx = state.buildContext();
            Q_UNUSED(ctx)
        }
        const double avgUs =
            timer.nsecsElapsed() / double(iterations) / 1000.0;
        qInfo().noquote() << QStringLiteral(
            "METRIC DocumentState capture+buildContext (1 МБ): %1 мкс/вызов")
                                 .arg(QString::number(avgUs, 'f', 2));
    }

    // (в) end-to-end: ручной запрос от вызова до ухода в backend.
    PlainTextEditorAdapter adapter(&editor);
    SuggestionController controller(&adapter);
    RecordingBackend backend;
    controller.setBackend(&backend);

    QElapsedTimer timer;
    timer.start();
    controller.requestSuggestion();
    const qint64 assemblyUs = timer.nsecsElapsed() / 1000;
    qInfo().noquote() << QStringLiteral(
        "METRIC requestAssembly(1 МБ, ручной запрос, весь путь до "
        "backend): %1 мкс")
                             .arg(assemblyUs);

    QCOMPARE(backend.requests.size(), 1);
    QVERIFY2(assemblyUs < 50'000,
             qPrintable(QStringLiteral(
                 "сборка запроса не должна блокировать UI > 50 мс: %1 мкс")
                            .arg(assemblyUs)));
}

// --- Проверка: нет избыточного контекста -------------------------------
// В модель уходит только префикс до курсора, обрезанный по cap
// DocumentState (2000 символов), — не весь документ и не хвост.
void PerformanceTest::requestPayloadStaysBounded()
{
    QPlainTextEdit editor;
    editor.setPlainText(repeatedToSize(
        QStringLiteral("Текст документа большой длины для проверки "
                       "объёма запроса. "),
        1024 * 1024));
    editor.moveCursor(QTextCursor::End);

    PlainTextEditorAdapter adapter(&editor);
    SuggestionController controller(&adapter);
    RecordingBackend backend;
    controller.setBackend(&backend);

    const int docChars = editor.toPlainText().size();

    // Курсор в конце документа: context == последние <=2000 символов.
    controller.requestSuggestion();
    QCOMPARE(backend.requests.size(), 1);
    const GenerationRequest first = backend.requests.at(0);
    controller.rejectSuggestion();

    qInfo().noquote() << QStringLiteral(
        "METRIC payload(1 МБ док., курсор в конце): context=%1 симв. "
        "(cap=%2), systemPrompt=%3 симв., styleWeights=%4 шт.")
                             .arg(first.context.size())
                             .arg(DocumentState::kDefaultContextLimitChars)
                             .arg(first.systemPrompt.size())
                             .arg(first.styleWeights.size());

    QVERIFY2(first.context.size()
                 <= DocumentState::kDefaultContextLimitChars + 1,
             "Префикс обязан обрезаться по cap DocumentState");
    QVERIFY2(first.context.size() * 4 < docChars,
             "В запрос не должен уходить заметный кусок документа");
    // Префикс — действительно хвост документа до курсора.
    QCOMPARE(first.context,
             editor.toPlainText().right(first.context.size()));
    // systemPrompt сейчас пуст: PromptBuilder в приложение ещё не
    // подключён (факт для отчёта, не ошибка объёма).
    QVERIFY(first.systemPrompt.isEmpty());

    // Курсор в середине: context — префикс до курсора, суффикс не уходит.
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(docChars / 2);
    editor.setTextCursor(cursor);
    controller.requestSuggestion();
    QCOMPARE(backend.requests.size(), 2);
    const GenerationRequest second = backend.requests.at(1);
    const int midPos = editor.textCursor().position();
    const QString prefix = editor.toPlainText().mid(
        midPos - second.context.size(), second.context.size());
    qInfo().noquote() << QStringLiteral(
        "METRIC payload(курсор в середине): context=%1 симв. — префикс, "
        "суффикс не отправляется")
                             .arg(second.context.size());
    QCOMPARE(second.context, prefix);
    QVERIFY(second.context.size()
            <= DocumentState::kDefaultContextLimitChars + 1);
}

// --- Метрика 4: отображение ghost suggestion ---------------------------
// Готовность (suggestionReady) -> первый paint overlay -> стоимость
// paint (grab). Окно показывается для реального рендера; фокус
// не требуется (ручной запрос).
void PerformanceTest::ghostDisplayLatencyAndPaintCost()
{
    QPlainTextEdit editor;
    editor.resize(360, 200);
    PlainTextEditorAdapter adapter(&editor);
    SuggestionController controller(&adapter);
    RecordingBackend backend;
    backend.setAutoRespond(5, QStringLiteral(" продолжение текста"));
    controller.setBackend(&backend);

    connect(&editor, &QPlainTextEdit::textChanged,
            &controller, &SuggestionController::onTextChanged);
    connect(&editor, &QPlainTextEdit::cursorPositionChanged,
            &controller, &SuggestionController::onCursorPositionChanged);

    SuggestionOverlay overlay(&editor);
    overlay.installEventFilter(this);

    m_clock.start();
    m_tReadyUs = -1;
    m_tFirstPaintUs = -1;
    connect(&controller, &SuggestionController::suggestionReady, this,
            [this, &overlay](const QString& text) {
                m_tReadyUs =
                    m_clock.nsecsElapsed() / 1000; // микросекунды
                overlay.setSuggestion(text);
            });
    connect(&controller, &SuggestionController::suggestionCleared,
            &overlay, &SuggestionOverlay::clear);

    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    editor.setPlainText(QStringLiteral("hello"));
    editor.moveCursor(QTextCursor::End);

    // Прогрев: шрифты/растеризация первого кадра — вне замера.
    m_watchPaint = false;
    controller.requestSuggestion();
    QVERIFY(QTest::qWaitFor([&controller]() {
        return controller.hasSuggestion();
    },
                            3000));
    QTest::qWait(50); // прогревочный paint доставлен
    controller.rejectSuggestion();
    QTest::qWait(20);

    // Измеряемый цикл.
    m_watchPaint = true;
    m_tReadyUs = -1;
    m_tFirstPaintUs = -1;
    controller.requestSuggestion();
    QVERIFY(QTest::qWaitFor([&controller]() {
        return controller.hasSuggestion();
    },
                            3000));
    QVERIFY2(QTest::qWaitFor(
                 [this]() { return m_tFirstPaintUs >= 0; }, 2000),
             "Overlay не получил paint после готовности подсказки");

    const qint64 displayUs = m_tFirstPaintUs - m_tReadyUs;
    qInfo().noquote() << QStringLiteral(
        "METRIC ghost display: suggestionReady -> первый paint = %1 мкс")
                             .arg(displayUs);
    QVERIFY2(m_paintCount >= 1, "paint не был пойман");
    QVERIFY2(displayUs < 250'000,
             qPrintable(QStringLiteral("отображение подсказки: %1 мкс")
                            .arg(displayUs)));

    // Стоимость самого paintEvent: grab() рисует синхронно.
    {
        QElapsedTimer timer;
        timer.start();
        const int iterations = 20;
        for (int i = 0; i < iterations; ++i)
            overlay.grab();
        const qint64 avgUs = timer.nsecsElapsed() / 1000 / iterations;
        qInfo().noquote() << QStringLiteral(
            "METRIC overlay paint(grab, 1 строка ghost): %1 мкс/кадр")
                                 .arg(avgUs);
        QVERIFY2(avgUs < 20'000,
                 qPrintable(QStringLiteral("дорогой кадр: %1 мкс")
                                .arg(avgUs)));
    }
}

// --- Проверка: UI не блокируется ---------------------------------------
// Сторожевой таймер 1 мс меряет крупнейший разрыв цикла событий во
// время полного сценария; отдельно — длительность синхронных
// операций контроллера.
void PerformanceTest::uiThreadStaysResponsiveDuringScenario()
{
    QPlainTextEdit editor;
    editor.resize(360, 200);
    PlainTextEditorAdapter adapter(&editor);
    SuggestionController controller(&adapter);
    RecordingBackend backend;
    backend.setAutoRespond(5, QStringLiteral(" подсказка"));
    controller.setBackend(&backend);

    connect(&editor, &QPlainTextEdit::textChanged,
            &controller, &SuggestionController::onTextChanged);
    connect(&editor, &QPlainTextEdit::cursorPositionChanged,
            &controller, &SuggestionController::onCursorPositionChanged);

    SuggestionOverlay overlay(&editor);
    connect(&controller, &SuggestionController::suggestionReady,
            &overlay, &SuggestionOverlay::setSuggestion);
    connect(&controller, &SuggestionController::suggestionCleared,
            &overlay, &SuggestionOverlay::clear);

    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));
    editor.setPlainText(QStringLiteral("warmup"));
    editor.moveCursor(QTextCursor::End);

    // Прогрев (первые аллокации/шрифты).
    controller.requestSuggestion();
    QVERIFY(QTest::qWaitFor([&controller]() {
        return controller.hasSuggestion();
    },
                            3000));
    controller.acceptSuggestion();
    QTest::qWait(30);

    StallWatchdog watchdog;
    watchdog.start();

    qint64 maxOpUs = 0;
    const auto measure = [&maxOpUs](const std::function<void()>& op) {
        QElapsedTimer timer;
        timer.start();
        op();
        const qint64 us = timer.nsecsElapsed() / 1000;
        if (us > maxOpUs)
            maxOpUs = us;
    };

    // Полный сценарий: печать -> запрос -> готовность -> принятие,
    // затем отмены «в полёте», всё с обработкой событий.
    for (int i = 0; i < 10; ++i) {
        measure([&editor]() { editor.insertPlainText(QStringLiteral("x")); });
        QTest::qWait(10);
        measure([&controller]() { controller.requestSuggestion(); });
        QVERIFY(QTest::qWaitFor([&controller]() {
            return controller.hasSuggestion();
        },
                                3000));
        if (i % 2 == 0) {
            measure([&controller]() { controller.acceptSuggestion(); });
        } else {
            measure([&controller]() { controller.requestSuggestion(); });
            measure([&controller]() { controller.rejectSuggestion(); });
        }
        QTest::qWait(20);
    }
    QTest::qWait(100);
    watchdog.stop();

    qInfo().noquote() << QStringLiteral(
        "METRIC UI: крупнейший разрыв цикла событий = %1 мс; "
        "крупнейшая синхронная операция = %2 мкс")
                             .arg(watchdog.maxGapMs())
                             .arg(maxOpUs);

    QVERIFY2(watchdog.maxGapMs() < 100,
             qPrintable(QStringLiteral("UI блокировался на %1 мс")
                            .arg(watchdog.maxGapMs())));
    QVERIFY2(maxOpUs < 50'000,
             qPrintable(QStringLiteral("синхронная операция %1 мкс")
                            .arg(maxOpUs)));

    controller.rejectSuggestion();
    editor.document()->setModified(false);
}

// --- Метрика 5: отмена -------------------------------------------------
// UI-часть отмены синхронна (откат состояния в том же вызове);
// поздний ответ backend'а отбрасывается по requestId.
void PerformanceTest::cancelLatencyAndStaleAnswerDrop()
{
    PerfFakeEditor editor;
    SuggestionController controller(&editor);
    RecordingBackend backend;
    backend.setAutoRespond(300, QStringLiteral(" поздний"));
    controller.setBackend(&backend);
    controller.setDebounceInterval(60000); // печать не мешает замеру

    connect(&editor, &PerfFakeEditor::textChanged,
            &controller, &SuggestionController::onTextChanged);
    connect(&editor, &PerfFakeEditor::cursorPositionChanged,
            &controller, &SuggestionController::onCursorPositionChanged);

    editor.typeText(QStringLiteral("hello"));
    controller.requestSuggestion();
    QCOMPARE(controller.state(),
             SuggestionController::State::Generating);

    QElapsedTimer clock;
    clock.start();
    controller.rejectSuggestion();
    const qint64 cancelUs = clock.nsecsElapsed() / 1000;
    qInfo().noquote() << QStringLiteral(
        "METRIC cancel(UI-часть): rejectSuggestion -> Idle = %1 мкс")
                             .arg(cancelUs);
    QCOMPARE(controller.state(), SuggestionController::State::Idle);
    QVERIFY2(cancelUs < 5000,
             qPrintable(QStringLiteral("отмена не мгновенна: %1 мкс")
                            .arg(cancelUs)));
    QCOMPARE(backend.cancelledIds.size(), 1);

    // Поздний ответ (через 300 мс) не воскресает подсказку.
    QTest::qWait(400);
    QVERIFY(!controller.hasSuggestion());
    QCOMPARE(controller.state(), SuggestionController::State::Idle);
}

// --- Проверки: QThread/QObject не накапливаются, память не растёт ------
// Живой MainWindow (llama отключён): циклы принятия и отмены через
// mock-бэкенд; до/после — счётчики потоков, объектов и RSS.
void PerformanceTest::mainWindowCyclesDoNotAccumulateObjectsOrMemory()
{
    MainWindow window;

    auto* controller = window.findChild<SuggestionController*>();
    QVERIFY(controller != nullptr);
    auto* editor = window.findChild<QPlainTextEdit*>(
        QStringLiteral("mainEditor"));
    QVERIFY(editor != nullptr);

    editor->setPlainText(QStringLiteral("hello"));
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);

    // Прогрев: первый цикл оплачивает шрифты/статусбар.
    controller->requestSuggestion();
    QVERIFY(QTest::qWaitFor([&controller]() {
        return controller->hasSuggestion();
    },
                            3000));
    QTest::qWait(50);

    const int threadsBefore = window.findChildren<QThread*>().size();
    const int objectsBefore = window.findChildren<QObject*>().size();
    const qint64 rssBefore = rssBytes();

    qInfo().noquote() << QStringLiteral(
        "METRIC resources(before): threads=%1, QObjects=%2, RSS=%3 КБ")
                             .arg(threadsBefore)
                             .arg(objectsBefore)
                             .arg(rssBefore / 1024);

    // 15 циклов «принять» (mock отвечает через 300 мс) + 15 отмен
    // «в полёте».
    const int cycles = 15;
    for (int i = 0; i < cycles; ++i) {
        controller->requestSuggestion();
        QVERIFY(QTest::qWaitFor([&controller]() {
            return controller->hasSuggestion();
        },
                                3000));
        controller->acceptSuggestion();
    }
    for (int i = 0; i < cycles; ++i) {
        controller->requestSuggestion();
        controller->rejectSuggestion();
        QTest::qWait(10);
    }
    QTest::qWait(400); // хвостовые таймеры mock'а отрабатывают

    const int threadsAfter = window.findChildren<QThread*>().size();
    const int objectsAfter = window.findChildren<QObject*>().size();
    const qint64 rssAfter = rssBytes();
    const qint64 rssGrowthKb =
        rssBefore > 0 && rssAfter > 0 ? (rssAfter - rssBefore) / 1024 : -1;

    qInfo().noquote() << QStringLiteral(
        "METRIC resources(after %1 циклов): threads=%2, QObjects=%3, "
        "RSS=%4 КБ, прирост=%5 КБ")
                             .arg(cycles * 2)
                             .arg(threadsAfter)
                             .arg(objectsAfter)
                             .arg(rssAfter / 1024)
                             .arg(rssGrowthKb);

    QCOMPARE(threadsAfter, threadsBefore);
    QVERIFY2(objectsAfter - objectsBefore <= 4,
             qPrintable(QStringLiteral("накопление QObject: %1 -> %2")
                            .arg(objectsBefore)
                            .arg(objectsAfter)));
    if (rssGrowthKb >= 0) {
        QVERIFY2(rssGrowthKb < 16 * 1024,
                 qPrintable(QStringLiteral("память выросла на %1 КБ")
                                .arg(rssGrowthKb)));
    }

    editor->document()->setModified(false);
    window.close();
    QVERIFY(!controller->isEnabled());
}

// --- Метрики 6–8: реальная модель (загрузка, фазы, TPS, память,
// отмена) ----------------------------------------------------------------
// Гейт PTUCH_MODEL_TESTS=1: единственный слот с GGUF. Модель
// загружается ОДИН раз; все дальнейшие запросы работают с ней же.
void PerformanceTest::realModelPhasesThroughputMemoryAndCancel()
{
    if (!qEnvironmentVariableIsSet("PTUCH_MODEL_TESTS"))
        QSKIP("Замеры с реальной GGUF включаются PTUCH_MODEL_TESTS=1");

    const QString modelPath = resolveModelPath();
    QVERIFY2(!modelPath.isEmpty(),
             "GGUF не найдена — запускать из каталога проекта (models/) "
             "или задать PTUCH_MODEL_PATH");

    QThread worker;
    worker.setObjectName(QStringLiteral("llamaWorker"));
    auto* backend = new LlamaBackend;
    backend->moveToThread(&worker);
    QObject::connect(&worker, &QThread::finished,
                     backend, &QObject::deleteLater);
    const auto stopWorker = qScopeGuard([backend, &worker]() {
        backend->requestStop();
        worker.quit();
        worker.wait(10000);
    });

    QSignalSpy loadedSpy(backend, &LlamaBackend::modelLoaded);
    QSignalSpy failedSpy(backend, &LlamaBackend::modelLoadFailed);
    QSignalSpy readySpy(backend,
                        &ITextGenerationBackend::generationReady);

    const qint64 rssBeforeLoad = rssBytes();

    backend->configure(modelPath, /*contextSize*/ 4096,
                       /*gpuLayers*/ -1);
    worker.start();
    QMetaObject::invokeMethod(backend, &LlamaBackend::loadModel,
                              Qt::QueuedConnection);
    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return loadedSpy.count() + failedSpy.count() > 0;
                 },
                 120000),
             "Загрузка модели не завершилась за 120 с");
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(loadedSpy.count(), 1);
    const qint64 loadMs = loadedSpy.first().at(2).toLongLong();
    const qint64 rssAfterLoad = rssBytes();
    qInfo().noquote() << QStringLiteral(
        "METRIC model: загрузка %1 мс, RSS %2 -> %3 МБ (прирост модели "
        "%4 МБ)")
                             .arg(loadMs)
                             .arg(rssBeforeLoad / (1024 * 1024))
                             .arg(rssAfterLoad / (1024 * 1024))
                             .arg((rssAfterLoad - rssBeforeLoad)
                                  / (1024 * 1024));

    StallWatchdog watchdog; // UI-поток = поток теста: он должен жить
    watchdog.start();

    // --- Типовой сценарий: 5 запросов, префикс ~250 символов, 48 токенов
    const QString typical = repeatedToSize(
        QStringLiteral("Пользователь печатает заметку: текст документа "
                       "продолжается естественно и плавно. "),
        250);

    qint64 sumPromptMs = 0;
    qint64 sumPromptTokens = 0;
    qint64 sumGenMs = 0;
    qint64 sumGenTokens = 0;
    qint64 sumTotalMs = 0;

    const int requests = 5;
    for (int i = 0; i < requests; ++i) {
        GenerationRequest request;
        request.requestId = quint64(i + 1);
        request.context = typical;
        request.maxTokens = 48;
        request.temperature = 0.7;
        request.topP = 0.3;

        backend->generate(request);
        const int expected = i + 1;
        QVERIFY(QTest::qWaitFor(
            [&readySpy, expected]() {
                return readySpy.count() >= expected;
            },
            60000));
        const GenerationResult result =
            readySpy.at(i).at(0).value<GenerationResult>();
        QCOMPARE(result.requestId, quint64(i + 1));
        QVERIFY(result.stopReason != QStringLiteral("cancelled"));

        const BackendDiagnostics diag = backend->diagnostics();
        const qint64 promptTps = diag.promptProcessingMs > 0
            ? diag.promptTokens * 1000 / diag.promptProcessingMs
            : 0;
        const qint64 tps = diag.generationMs > 0
            ? diag.generatedTokens * 1000 / diag.generationMs
            : 0;
        const qint64 residualMs =
            result.elapsedMs - diag.promptProcessingMs - diag.generationMs;

        qInfo().noquote() << QStringLiteral(
            "METRIC req%1: total=%2 мс | prompt=%3 ток. за %4 мс "
            "(%5 ток/с) | gen=%6 ток. за %7 мс (%8 ток/с) | "
            "прочее=%9 мс")
                                 .arg(i + 1)
                                 .arg(result.elapsedMs)
                                 .arg(diag.promptTokens)
                                 .arg(diag.promptProcessingMs)
                                 .arg(promptTps)
                                 .arg(diag.generatedTokens)
                                 .arg(diag.generationMs)
                                 .arg(tps)
                                 .arg(residualMs);

        sumPromptMs += diag.promptProcessingMs;
        sumPromptTokens += diag.promptTokens;
        sumGenMs += diag.generationMs;
        sumGenTokens += diag.generatedTokens;
        sumTotalMs += result.elapsedMs;
    }

    qInfo().noquote() << QStringLiteral(
        "METRIC summary(%1 запросов): сумма total=%2 мс | "
        "prompt: %3 ток. за %4 мс (%5 ток/с) | "
        "gen: %6 ток. за %7 мс — средний TPS=%8")
                             .arg(requests)
                             .arg(sumTotalMs)
                             .arg(sumPromptTokens)
                             .arg(sumPromptMs)
                             .arg(sumPromptMs > 0
                                      ? sumPromptTokens * 1000
                                            / sumPromptMs
                                      : 0)
                             .arg(sumGenTokens)
                             .arg(sumGenMs)
                             .arg(sumGenMs > 0
                                      ? sumGenTokens * 1000 / sumGenMs
                                      : 0);

    // --- Промпт >512 токенов: цена второго чанка (finding F1) -----------
    // kPromptBatchSize=512: первый чанк декодируется быстро, каждый
    // следующий в llama.cpp b11046 на M1/Metal занимает ~2 с (замерено:
    // см. PERFORMANCE_REVIEW.md). id 90 — ~850 токенов (2 чанка),
    // завершается штатно; id 91 — типовой сразу после: состояние после
    // ЗАВЕРШЁННОГО большого запроса чистое (не отравлено).
    {
        GenerationRequest multiChunk;
        multiChunk.requestId = 90;
        multiChunk.context = repeatedToSize(
            QStringLiteral("Пользователь печатает заметку: текст документа "
                           "продолжается естественно и плавно. "),
            3000);
        multiChunk.maxTokens = 16;
        multiChunk.temperature = 0.7;
        multiChunk.topP = 0.3;
        backend->generate(multiChunk);
        QVERIFY(QTest::qWaitFor(
            [&readySpy]() { return readySpy.count() >= requests + 1; },
            60000));
        const GenerationResult bigResult =
            readySpy.at(requests).at(0).value<GenerationResult>();
        const BackendDiagnostics bigDiag = backend->diagnostics();
        qInfo().noquote() << QStringLiteral(
            "METRIC большой промпт (2 чанка, без отмены): total=%1 мс | "
            "prompt=%2 ток. за %3 мс (ожидается ~2-я чанк ≈ 2000 мс)")
                                 .arg(bigResult.elapsedMs)
                                 .arg(bigDiag.promptTokens)
                                 .arg(bigDiag.promptProcessingMs);
        QVERIFY(bigResult.stopReason != QStringLiteral("cancelled"));
        QVERIFY2(bigResult.elapsedMs < 15000,
                 "Большой промпт завис — ответа дождались за 15 с");

        GenerationRequest followUp;
        followUp.requestId = 91;
        followUp.context = typical;
        followUp.maxTokens = 16;
        followUp.temperature = 0.7;
        followUp.topP = 0.3;
        backend->generate(followUp);
        QVERIFY(QTest::qWaitFor(
            [&readySpy]() { return readySpy.count() >= requests + 2; },
            60000));
        const BackendDiagnostics afterBigDiag = backend->diagnostics();
        qInfo().noquote() << QStringLiteral(
            "METRIC типовой после большого: total=%1 мс | prompt=%2 ток. "
            "за %3 мс")
                                 .arg(readySpy.at(requests + 1)
                                          .at(0)
                                          .value<GenerationResult>()
                                          .elapsedMs)
                                 .arg(afterBigDiag.promptTokens)
                                 .arg(afterBigDiag.promptProcessingMs);
        QVERIFY2(afterBigDiag.promptProcessingMs < 500,
                 "После завершённого большого запроса типовой замедлился");

        // id 92: чистая отмена во время генерации (пока состояние чистое) —
        // основная метрика отмены; id 93: типовой запрос после неё.
        GenerationRequest cleanCancel;
        cleanCancel.requestId = 92;
        cleanCancel.context = typical;
        cleanCancel.maxTokens = 512;
        cleanCancel.temperature = 0.7;
        cleanCancel.topP = 0.3;
        backend->generate(cleanCancel);
        QTest::qWait(600); // prefill ~30 мс — генерация уже идёт
        QElapsedTimer expClock;
        expClock.start();
        backend->cancel(92);
        QVERIFY(QTest::qWaitFor(
            [&readySpy]() { return readySpy.count() >= requests + 3; },
            60000));
        const GenerationResult cleanResult =
            readySpy.at(requests + 2).at(0).value<GenerationResult>();
        const qint64 cleanCancelMs = expClock.nsecsElapsed() / 1000000;
        qInfo().noquote() << QStringLiteral(
            "METRIC отмена генерации (чистая): сигнал в +600 мс, ответ "
            "через %1 мс, stopReason=%2")
                                 .arg(cleanCancelMs)
                                 .arg(cleanResult.stopReason);
        QCOMPARE(cleanResult.stopReason, QStringLiteral("cancelled"));
        QVERIFY2(cleanCancelMs < 1000,
                 "Отмена во время генерации слишком медленная");

        GenerationRequest afterCleanCancel;
        afterCleanCancel.requestId = 93;
        afterCleanCancel.context = typical;
        afterCleanCancel.maxTokens = 16;
        afterCleanCancel.temperature = 0.7;
        afterCleanCancel.topP = 0.3;
        backend->generate(afterCleanCancel);
        QVERIFY(QTest::qWaitFor(
            [&readySpy]() { return readySpy.count() >= requests + 4; },
            60000));
        const BackendDiagnostics afterCancelDiag = backend->diagnostics();
        qInfo().noquote() << QStringLiteral(
            "METRIC типовой после чистой отмены: total=%1 мс | prompt=%2 "
            "ток. за %3 мс")
                                 .arg(readySpy.at(requests + 3)
                                              .at(0)
                                              .value<GenerationResult>()
                                              .elapsedMs)
                                 .arg(afterCancelDiag.promptTokens)
                                 .arg(afterCancelDiag.promptProcessingMs);
        QVERIFY2(afterCancelDiag.promptProcessingMs < 500,
                 "После чистой отмены генерации типовой запрос замедлился");
    }
    const int spyBase = readySpy.count(); // 5 + 4 (большой/после/отмены)

    // --- Отмена во время prompt processing (2-й чанк ≈ 2 с, F1) ---------
    // Отмена сигналится в +120 мс, пока worker ждёт медленный 2-й чанк;
    // ответ приходит сразу после его завершения (непрерываемый llama_decode).
    GenerationRequest prefillRequest;
    prefillRequest.requestId = 100;
    prefillRequest.context = repeatedToSize(
        QStringLiteral("Очень длинный контекст документа перед курсором: "
                       "много абзацев, слов и строк. "),
        7000);
    prefillRequest.maxTokens = 512;
    backend->generate(prefillRequest);
    QTest::qWait(120); // уходим в prefill (чанки по 512 токенов)
    QElapsedTimer clock;
    clock.start();
    backend->cancel(100);
    QVERIFY(QTest::qWaitFor([&readySpy, spyBase]() {
        return readySpy.count() >= spyBase + 1;
    },
                            60000));
    const qint64 prefillCancelUs = clock.nsecsElapsed() / 1000;
    const GenerationResult prefillResult =
        readySpy.at(spyBase).at(0).value<GenerationResult>();
    qInfo().noquote() << QStringLiteral(
        "METRIC cancel во время prefill (F1, 2-й чанк): сигнал в +120 мс, "
        "ответ через %1 мс, stopReason=%2, prompt обработан за %3 мс")
                             .arg(prefillCancelUs / 1000)
                             .arg(prefillResult.stopReason)
                             .arg(backend->diagnostics()
                                      .promptProcessingMs);
    QCOMPARE(prefillResult.stopReason, QStringLiteral("cancelled"));
    QVERIFY2(prefillCancelUs / 1000 < 3000,
             "Отмена во время prefill слишком медленная");

    // --- Отмена при «отравленном» промпте (finding F2) -------------------
    // После отменённого мультичанкового промпта (id 100) первый чанк
    // следующего запроса тоже декодируется ~2 с (F2, см. PERFORMANCE_REVIEW).
    // Сигнал отмены в +600 мс попадает внутрь этого декода — важно, что
    // ответ приходит с stopReason=cancelled, а не теряется.
    GenerationRequest genRequest;
    genRequest.requestId = 101;
    genRequest.context = typical;
    genRequest.maxTokens = 512;
    backend->generate(genRequest);
    QTest::qWait(600); // в чистом состоянии генерация уже шла бы тут
    clock.restart();
    backend->cancel(101);
    QVERIFY(QTest::qWaitFor([&readySpy, spyBase]() {
        return readySpy.count() >= spyBase + 2;
    },
                            60000));
    const qint64 genCancelUs = clock.nsecsElapsed() / 1000;
    const GenerationResult genResult =
        readySpy.at(spyBase + 1).at(0).value<GenerationResult>();
    qInfo().noquote() << QStringLiteral(
        "METRIC cancel после отменённого промпта (F2): ответ через %1 мс, "
        "stopReason=%2")
                             .arg(genCancelUs / 1000)
                             .arg(genResult.stopReason);
    QCOMPARE(genResult.stopReason, QStringLiteral("cancelled"));
    QVERIFY2(genCancelUs / 1000 < 5000,
             "Отмена при отравленном промпте не дождалась ответа за 5 с");

    // --- Восстановление после F2: типовой запрос сразу после ------------
    GenerationRequest recovery;
    recovery.requestId = 104;
    recovery.context = typical;
    recovery.maxTokens = 16;
    recovery.temperature = 0.7;
    recovery.topP = 0.3;
    backend->generate(recovery);
    QVERIFY(QTest::qWaitFor([&readySpy, spyBase]() {
        return readySpy.count() >= spyBase + 3;
    },
                            60000));
    const BackendDiagnostics recoveryDiag = backend->diagnostics();
    const GenerationResult recoveryResult =
        readySpy.at(spyBase + 2).at(0).value<GenerationResult>();
    qInfo().noquote() << QStringLiteral(
        "METRIC типовой после отменённого промпта (F2): total=%1 мс | "
        "prompt=%2 ток. за %3 мс")
                             .arg(recoveryResult.elapsedMs)
                             .arg(recoveryDiag.promptTokens)
                             .arg(recoveryDiag.promptProcessingMs);
    QVERIFY2(recoveryDiag.promptProcessingMs < 5000,
             "Запрос после отравленного завис намертво (>5 с)");

    watchdog.stop();
    qInfo().noquote() << QStringLiteral(
        "METRIC UI во время генерации: крупнейший разрыв цикла событий "
        "= %1 мс")
                             .arg(watchdog.maxGapMs());
    QVERIFY2(watchdog.maxGapMs() < 150,
             qPrintable(QStringLiteral("UI блокировался на %1 мс")
                            .arg(watchdog.maxGapMs())));

    // Модель пересоздавалась? Ровно один modelLoaded на все 12 запросов
    // (5 типовых + 4 диагностики + 2 отмены + 1 восстановление).
    QCOMPARE(loadedSpy.count(), 1);

    const qint64 rssFinal = rssBytes();
    const qint64 rssGrowthMb = (rssFinal - rssAfterLoad) / (1024 * 1024);
    qInfo().noquote() << QStringLiteral(
        "METRIC memory после 12 запросов: RSS %1 -> %2 МБ "
        "(прирост %3 МБ)")
                             .arg(rssAfterLoad / (1024 * 1024))
                             .arg(rssFinal / (1024 * 1024))
                             .arg(rssGrowthMb);
    // Измерено: однократный прирост 25–27 МБ после серии (похоже на рост
    // ggml-буферов под первый большой граф), не зависит от числа прогонов.
    // Граница 64 МБ улавливает по-настоящему пошаговую утечку (12 запросов
    // × ≥6 МБ), не мигая на шуме RSS.
    QVERIFY2(rssGrowthMb < 64,
             qPrintable(QStringLiteral("память выросла на %1 МБ")
                            .arg(rssGrowthMb)));
}

QTEST_MAIN(PerformanceTest)

#include "performance_test.moc"
