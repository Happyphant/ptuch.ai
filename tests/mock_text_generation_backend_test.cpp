// mock_text_generation_backend_test.cpp
#include <QtTest>

#include "backend/mock_text_generation_backend.h"

#include <QCoreApplication>
#include <QThread>

#include <atomic>

// Тесты MockTextGenerationBackend: работа вне UI-потока, задержка,
// имитация ошибки, отмена запроса (в двух режимах: честном и «плохом»).
class MockTextGenerationBackendTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void returnsResultAfterDelayInWorkerThread();
    void concurrentRequestsKeepTheirRequestIds();
    void errorSimulation();
    void cancelSuppressesResponse();
    void ignoreCancelStillResponds();

private:
    QThread* thread = nullptr;
    MockTextGenerationBackend* backend = nullptr;
    QSignalSpy* readySpy = nullptr;
    QSignalSpy* errorSpy = nullptr;
    std::atomic_bool m_answeredFromWorker{false};
};

void MockTextGenerationBackendTest::init()
{
    m_answeredFromWorker.store(false);

    thread = new QThread;
    thread->setObjectName(QStringLiteral("mockGenerationThread"));

    // Без родителя: иначе moveToThread не сработает. Освобождение —
    // по finished потока (канонический паттерн Qt).
    backend = new MockTextGenerationBackend;
    backend->setDelayMs(200);
    backend->moveToThread(thread);

    // Настройки — до старта потока (setters не потокобезопасны).
    // Тесты вызывают thread->start() уже после своих setXxx().

    connect(thread, &QThread::finished,
            backend, &QObject::deleteLater);

    readySpy = new QSignalSpy(
        backend, &ITextGenerationBackend::generationReady);
    errorSpy = new QSignalSpy(
        backend, &ITextGenerationBackend::generationError);
}

void MockTextGenerationBackendTest::cleanup()
{
    if (thread->isRunning()) {
        thread->quit();
        thread->wait(3000);
        // backend удаляется в потоке через finished -> deleteLater.
    } else {
        // Поток не запускался — удаляем вручную.
        delete backend;
    }
    backend = nullptr;

    delete readySpy;
    delete errorSpy;
    delete thread;
}

void MockTextGenerationBackendTest::returnsResultAfterDelayInWorkerThread()
{
    // Прямое подключение => лямбда выполняется в потоке, откуда
    // пришёл сигнал: фиксируем, что это НЕ UI-поток.
    connect(backend, &ITextGenerationBackend::generationReady, this,
            [this](const GenerationResult&) {
                m_answeredFromWorker.store(
                    QThread::currentThread() !=
                    QCoreApplication::instance()->thread());
            },
            Qt::DirectConnection);

    thread->start();

    GenerationRequest request;
    request.requestId = 42;
    request.context = QStringLiteral("hello ");
    backend->generate(request);

    // До задержки ответа нет (UI при этом не заблокирован).
    QTest::qWait(50);
    QCOMPARE(readySpy->count(), 0);

    QVERIFY(QTest::qWaitFor(
        [this]() { return readySpy->count() == 1; }, 3000));

    const GenerationResult result =
        readySpy->at(0).at(0).value<GenerationResult>();
    QCOMPARE(result.requestId, quint64(42));
    QVERIFY(!result.generatedText.isEmpty());
    QCOMPARE(result.stopReason, QStringLiteral("stop"));
    // Задержка ~200 мс: результат не «мгновенный».
    QVERIFY(result.elapsedMs >= 150);
    QVERIFY(m_answeredFromWorker.load());
}

void MockTextGenerationBackendTest::concurrentRequestsKeepTheirRequestIds()
{
    thread->start();

    GenerationRequest first;
    first.requestId = 1;
    first.context = QStringLiteral("alpha ");

    GenerationRequest second;
    second.requestId = 2;
    second.context = QStringLiteral("beta ");

    backend->generate(first);
    backend->generate(second);

    QVERIFY(QTest::qWaitFor(
        [this]() { return readySpy->count() == 2; }, 3000));

    // Явный requestId: каждый ответ несёт свой id.
    QSet<quint64> ids;
    for (const auto& args : *readySpy)
        ids.insert(args.at(0).value<GenerationResult>().requestId);

    const QSet<quint64> expected{quint64(1), quint64(2)};
    QCOMPARE(ids, expected);
}

void MockTextGenerationBackendTest::errorSimulation()
{
    backend->setSimulateError(true); // до старта потока
    thread->start();

    GenerationRequest request;
    request.requestId = 7;
    request.context = QStringLiteral("boom");
    backend->generate(request);

    QVERIFY(QTest::qWaitFor(
        [this]() { return errorSpy->count() == 1; }, 3000));

    QCOMPARE(readySpy->count(), 0);
    QCOMPARE(errorSpy->at(0).at(0).toULongLong(), quint64(7));
    QVERIFY(!errorSpy->at(0).at(1).toString().isEmpty());
}

void MockTextGenerationBackendTest::cancelSuppressesResponse()
{
    thread->start();

    GenerationRequest request;
    request.requestId = 5;
    request.context = QStringLiteral("hello");
    backend->generate(request);

    QTest::qWait(20); // generate() обработан в потоке объекта
    backend->cancel(5);

    // Ждём дольше задержки: отменённый запрос игнорируется —
    // ни ready, ни error.
    QTest::qWait(500);
    QCOMPARE(readySpy->count(), 0);
    QCOMPARE(errorSpy->count(), 0);
}

void MockTextGenerationBackendTest::ignoreCancelStillResponds()
{
    backend->setIgnoreCancel(true); // до старта потока
    thread->start();

    GenerationRequest request;
    request.requestId = 9;
    request.context = QStringLiteral("hello");
    backend->generate(request);

    QTest::qWait(20);
    backend->cancel(9);

    // «Плохой» backend: отмену проигнорировал и всё равно ответил —
    // потребитель обязан отбросить такой ответ по requestId.
    QVERIFY(QTest::qWaitFor(
        [this]() { return readySpy->count() == 1; }, 3000));

    const GenerationResult result =
        readySpy->at(0).at(0).value<GenerationResult>();
    QCOMPARE(result.requestId, quint64(9));
    QCOMPARE(result.stopReason, QStringLiteral("cancelled"));
}

QTEST_GUILESS_MAIN(MockTextGenerationBackendTest)

#include "mock_text_generation_backend_test.moc"
