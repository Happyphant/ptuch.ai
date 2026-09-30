// test_plan_test.cpp
// Сквозные сценарии тестового плана (TEST_PLAN.md), которым нужен живой
// MainWindow, но НЕ фокус окна и НЕ реальная модель/GPU:
//
//  1) ошибка загрузки модели: битый GGUF из PTUCH_MODEL_PATH ->
//     modelLoadFailed -> статусная строка с причиной, контроллер
//     остаётся на mock-бэкенде и подсказки продолжают работать;
//  2) закрытие приложения посреди активной генерации: closeEvent ->
//     shutdown (debounce стоп, cancel, отвязка backend) + остановка
//     потоков в деструкторе; поздний ответ mock'а отброшен.
//
// Детерминизм: PTUCH_MODEL_PATH всегда указывает на заведомо битый
// файл (падение загрузки не зависит от содержимого models/ и от
// наличия GPU); тайминги — только через waitFor/qWait с запасом.
// Внешние PTUCH_DISABLE_LLAMA / PTUCH_MODEL_PATH в сняты и
// восстанавливаются в cleanupTestCase.
#include <QtTest>

#include "UI/MainWindow.h"
#include "backend/mock_text_generation_backend.h"
#include "suggestion/suggestion_controller.h"

#include <QFile>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>

class TestPlanTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void modelLoadErrorFallsBackToMockAndSuggestionsStillWork();
    void closeDuringGenerationStopsWorkerAndDropsLateAnswer();

private:
    QTemporaryDir m_modelDir;        // каталог битой «модели»
    QByteArray m_previousModelPath;  // прежнее PTUCH_MODEL_PATH
    bool m_hadModelPath = false;
};

void TestPlanTest::initTestCase()
{
    QVERIFY(m_modelDir.isValid());

    // Заведомо битый GGUF: ошибка загрузки детерминирована и не
    // требует ни реальной модели, ни GPU.
    const QString badPath =
        m_modelDir.filePath(QStringLiteral("corrupt.gguf"));
    QFile badFile(badPath);
    QVERIFY(badFile.open(QIODevice::WriteOnly));
    badFile.write("this is definitely not a gguf model");
    badFile.close();

    // План требует РЕАЛЬНУЮ попытку загрузки: внешнее отключение
    // llama снимаем, внешний путь к модели — заменяем и сохраняем.
    qunsetenv("PTUCH_DISABLE_LLAMA");
    m_hadModelPath = qEnvironmentVariableIsSet("PTUCH_MODEL_PATH");
    if (m_hadModelPath)
        m_previousModelPath = qgetenv("PTUCH_MODEL_PATH");
    qputenv("PTUCH_MODEL_PATH", badPath.toUtf8());
}

void TestPlanTest::cleanupTestCase()
{
    if (m_hadModelPath)
        qputenv("PTUCH_MODEL_PATH", m_previousModelPath);
    else
        qunsetenv("PTUCH_MODEL_PATH");
}

// Пункт плана «ошибка загрузки модели» на уровне приложения.
void TestPlanTest::modelLoadErrorFallsBackToMockAndSuggestionsStillWork()
{
    // Окно не показываем: статусная строка и ручной requestSuggestion
    // работают без активации — macOS-фокус в тесте не участвует.
    MainWindow window;

    // 1) Битый GGUF -> modelLoadFailed -> статус с причиной и явным
    //    «остаёмся на mock» (загрузка в фоне, UI не блокирован).
    QStatusBar* status = window.statusBar();
    QVERIFY(status != nullptr);
    QVERIFY2(
        QTest::qWaitFor(
            [&]() {
                const QString message = status->currentMessage();
                return message.startsWith(QStringLiteral("llama.cpp:"))
                    && message.contains(
                        QStringLiteral("остаёмся на mock"));
            },
            15000),
        qPrintable(status->currentMessage()));

    // 2) DI-подмены не было: контроллер по-прежнему на mock.
    auto* controller = window.findChild<SuggestionController*>();
    QVERIFY(controller != nullptr);
    QVERIFY2(
        qobject_cast<MockTextGenerationBackend*>(
            controller->backend())
            != nullptr,
        "После ошибки загрузки контроллер должен остаться на mock");

    // 3) Подсказки продолжают работать: контекст -> mock -> Ready.
    auto* editor = window.findChild<QPlainTextEdit*>(
        QStringLiteral("mainEditor"));
    QVERIFY(editor != nullptr);
    editor->setPlainText(QStringLiteral("hello"));
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);

    controller->requestSuggestion(); // ручной: минует debounce, без фокуса
    QCOMPARE(controller->state(),
             SuggestionController::State::Generating);
    QVERIFY(QTest::qWaitFor(
        [&]() { return controller->hasSuggestion(); }, 5000));
    QCOMPARE(controller->state(), SuggestionController::State::Ready);

    // Детерминированный ответ mock'а — автоподсказка по контексту.
    QCOMPARE(controller->suggestion(),
             QStringLiteral(" continuation of \"hello\""));

    // 4) Чистое закрытие: modified погашен -> без диалога, shutdown
    //    выполнен (closeEvent), потоки остановит деструктор.
    editor->document()->setModified(false);
    window.close();
    QVERIFY(!controller->isEnabled());
    QVERIFY(controller->backend() == nullptr);
}

// Пункт плана «закрытие приложения во время генерации» на уровне
// приложения (unit-уровень — слот shutdownDuringGenerationDropsLateAnswer
// в SuggestionControllerTests, UI с фокусом — ctrlSpaceAndFocusOutScenario).
void TestPlanTest::closeDuringGenerationStopsWorkerAndDropsLateAnswer()
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

    // Генерация «в полёте»: mock отвечает через 300 мс, ответ ещё не
    // приходит.
    controller->requestSuggestion();
    QCOMPARE(controller->state(),
             SuggestionController::State::Generating);

    editor->document()->setModified(false);
    window.close();

    // Shutdown синхронно в closeEvent: debounce/запрос остановлены,
    // backend отвязан.
    QVERIFY(!controller->isEnabled());
    QVERIFY(controller->backend() == nullptr);

    // Поздний ответ mock'а (спустя задержку) отброшен: подсказки нет,
    // состояние остаётся Idle. Деструктор окна остановит потоки
    // mock и llama (повторный stopLlamaWorker — no-op).
    QTest::qWait(600);
    QVERIFY(!controller->hasSuggestion());
    QCOMPARE(controller->state(), SuggestionController::State::Idle);
}

QTEST_MAIN(TestPlanTest)

#include "test_plan_test.moc"
