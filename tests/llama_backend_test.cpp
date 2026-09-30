// Тесты LlamaBackend (ITextGenerationBackend поверх llama.cpp):
//  1) контракт без модели — быстро, всегда в ctest: поиск GGUF, ошибка
//     «модель не загружена», асинхронный вызов из чужого потока,
//     безопасная отмена неизвестного id;
//  2) интеграционный smoke с реальной моделью — включается
//     PTUCH_MODEL_TESTS=1 (вне ctest по умолчанию, чтобы не грузить
//     2 ГБ GGUF на каждый прогон).
#include <QtTest>

#include "llama/llama_backend.h"

#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>

class LlamaBackendTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void resolveModelPath_prefersEnvPath();
    void resolveModelPath_ignoresMissingEnvPath();
    void generate_withoutModel_reportsError();
    void generate_withAdapter_reportsUnsupported();
    void generate_fromForeignThread_isAsync();
    void cancel_unknownId_keepsBackendUsable();
    void diagnostics_reportsModelInfo();
    void modelSmoke_realGguf();

private:
    LlamaBackend* m_backend = nullptr;
};

void LlamaBackendTest::init()
{
    // Объект в потоке теста: без moveToThread generate() идёт
    // синхронно — удобно для проверок ошибок без модели.
    m_backend = new LlamaBackend;
}

void LlamaBackendTest::cleanup()
{
    if (m_backend == nullptr)
        return;

    // Поточные тесты обнуляют указатель в scope-guard'е (после
    // quit/wait); здесь остаются только объекты потока теста.
    if (m_backend->thread() != QThread::currentThread()) {
        m_backend->requestStop();
        m_backend->thread()->quit();
        m_backend->thread()->wait(10000);
    }
    delete m_backend;
    m_backend = nullptr;
}

void LlamaBackendTest::resolveModelPath_prefersEnvPath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString model = dir.filePath(QStringLiteral("test.gguf"));
    QFile file(model);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    // env — высший приоритет: путь возвращается как есть (нормализуется
    // в абсолютный), независимо от настроек и папки models/.
    qputenv("PTUCH_MODEL_PATH", model.toUtf8());
    const QString resolved = resolveModelPath();
    qunsetenv("PTUCH_MODEL_PATH");

    QCOMPARE(resolved, QFileInfo(model).absoluteFilePath());
}

void LlamaBackendTest::resolveModelPath_ignoresMissingEnvPath()
{
    // Не существует -> падает в более низкий уровень поиска, но никак
    // не возвращает заведомо несуществующий путь.
    const char* missing = "/nonexistent/definitely_missing.gguf";
    qputenv("PTUCH_MODEL_PATH", missing);
    const QString resolved = resolveModelPath();
    qunsetenv("PTUCH_MODEL_PATH");

    QVERIFY2(resolved != QString::fromLatin1(missing),
             "Не должен возвращать несуществующий путь из окружения");
}

void LlamaBackendTest::generate_withoutModel_reportsError()
{
    QSignalSpy errorSpy(m_backend,
                        &ITextGenerationBackend::generationError);

    GenerationRequest request;
    request.requestId = 42;
    // Поток теста == поток объекта -> синхронный путь (в приложении
    // из UI-потока была бы queued-диспетчеризация).
    m_backend->generate(request);

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().at(0).toULongLong(), quint64(42));
    const QString message = errorSpy.first().at(1).toString();
    QVERIFY2(message.contains(QStringLiteral("не загружена")),
             qPrintable(message));

    // Диагностика: ошибка генерации зафиксирована в снимке — текстом,
    // без указателей/адресов (так и показывается в UI).
    const BackendDiagnostics diag = m_backend->diagnostics();
    QVERIFY2(diag.lastError.contains(QStringLiteral("не загружена")),
             qPrintable(diag.lastError));
}

// Адаптеры (LoRA) не реализованы: контракт интерфейса — при
// supportsAdapters()==false бэкенд отвечает ЯВНОЙ ошибкой на
// adapterPath, а не тихо игнорирует и не «грузит» ничего фиктивного.
// Отказ идёт ДО загрузки модели и без llama-вызовов (см. README
// «Стили и LoRA-адаптеры»).
void LlamaBackendTest::generate_withAdapter_reportsUnsupported()
{
    QVERIFY(!m_backend->supportsAdapters());

    QSignalSpy errorSpy(m_backend,
                        &ITextGenerationBackend::generationError);

    GenerationRequest request;
    request.requestId = 77;
    request.context = QStringLiteral("hello");
    request.adapter.path = QStringLiteral("/models/style-lora.safetensors");
    request.adapter.baseModelId = QStringLiteral("qwen2.5-3b");

    m_backend->generate(request);

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().at(0).toULongLong(), quint64(77));
    const QString message = errorSpy.first().at(1).toString();
    QVERIFY2(message.contains(QStringLiteral("не поддерживает адаптеры")),
             qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("style-lora.safetensors")),
             qPrintable(message));

    // Ни загрузки модели, ни фиктивной генерации.
    QVERIFY(!m_backend->isModelLoaded());

    // Ошибка отражена в снимке диагностики (её и показывает UI).
    const BackendDiagnostics diag = m_backend->diagnostics();
    QVERIFY2(diag.lastError.contains(
                 QStringLiteral("не поддерживает адаптеры")),
             qPrintable(diag.lastError));
}

void LlamaBackendTest::generate_fromForeignThread_isAsync()
{
    QThread worker;
    worker.setObjectName(QStringLiteral("llamaWorker"));
    m_backend->moveToThread(&worker);
    QObject::connect(&worker, &QThread::finished,
                     m_backend, &QObject::deleteLater);
    // При раннем выходе (QVERIFY) гасим поток ДО разрушения локальных
    // объектов; m_backend обнуляется — cleanup() его не трогает.
    const auto stopWorker = qScopeGuard([this, &worker]() {
        m_backend->requestStop();
        worker.quit();
        worker.wait(10000);
        m_backend = nullptr; // освобождён deleteLater по finished
    });

    QSignalSpy errorSpy(m_backend,
                        &ITextGenerationBackend::generationError);
    worker.start();

    GenerationRequest request;
    request.requestId = 7;
    m_backend->generate(request); // из чужого потока — не должно ждать

    // Ответа ещё нет: работа перенесена queued-событием в worker.
    // (Без переноса ошибка была бы уже здесь — UI-поток не должен
    // делать llama-вызовы и блокироваться.)
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY2(errorSpy.wait(5000), "Ответ worker-потока не пришёл");
    QCOMPARE(errorSpy.first().at(0).toULongLong(), quint64(7));
}

void LlamaBackendTest::cancel_unknownId_keepsBackendUsable()
{
    // Отмена неактивного id — «вежливость»: флаг не ставится, никаких
    // llama-вызовов; следующий запрос работает как обычно.
    m_backend->cancel(12345);
    m_backend->cancel(0);

    QSignalSpy errorSpy(m_backend,
                        &ITextGenerationBackend::generationError);
    GenerationRequest request;
    request.requestId = 9;
    m_backend->generate(request);

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().at(0).toULongLong(), quint64(9));
}

void LlamaBackendTest::diagnostics_reportsModelInfo()
{
    // configure() фиксирует имя модели и параметры — уже без загрузки
    // (файл не создаётся и не читается): имя в снимке — только файл GGUF.
    m_backend->configure(QStringLiteral("/tmp/some-model.gguf"),
                         /*contextSize*/ 4096, /*gpuLayers*/ -1);

    const BackendDiagnostics diag = m_backend->diagnostics();
    QCOMPARE(diag.backendName, QStringLiteral("llama.cpp"));
    QCOMPARE(diag.modelName, QStringLiteral("some-model.gguf"));
    QCOMPARE(diag.contextSize, 4096);
    QCOMPARE(diag.gpuLayers, -1);
    QVERIFY(diag.lastError.isEmpty());
    // Генерации ещё не было — метрики нулевые (UI покажет «—»).
    QCOMPARE(diag.promptTokens, qint64(0));
    QCOMPARE(diag.generatedTokens, qint64(0));
    QCOMPARE(diag.generationMs, qint64(0));
}

void LlamaBackendTest::modelSmoke_realGguf()
{
    if (!qEnvironmentVariableIsSet("PTUCH_MODEL_TESTS"))
        QSKIP("Smoke с реальной GGUF включается PTUCH_MODEL_TESTS=1");

    const QString modelPath = resolveModelPath();
    QVERIFY2(!modelPath.isEmpty(), "GGUF не найдена (models/ или настройки)");

    QThread worker;
    worker.setObjectName(QStringLiteral("llamaWorker"));
    m_backend->moveToThread(&worker);
    QObject::connect(&worker, &QThread::finished,
                     m_backend, &QObject::deleteLater);
    const auto stopWorker = qScopeGuard([this, &worker]() {
        m_backend->requestStop();
        worker.quit();
        worker.wait(10000);
        m_backend = nullptr;
    });

    // Запись выполняется в потоке исполнения llama-контекста (прямое
    // подключение к backend'у), чтение — после qWaitFor: порядок
    // гарантирован очередью событий, когда сигнал уже доставлен.
    QThread* loadedThread = nullptr;
    QObject::connect(m_backend, &LlamaBackend::modelLoaded, m_backend,
                     [&loadedThread](const QString&, int, qint64) {
                         loadedThread = QThread::currentThread();
                     },
                     Qt::DirectConnection);

    QSignalSpy loadedSpy(m_backend, &LlamaBackend::modelLoaded);
    QSignalSpy failedSpy(m_backend, &LlamaBackend::modelLoadFailed);

    // Конфигурация до старта потока; загрузка — queued в worker.
    m_backend->configure(modelPath, /*contextSize*/ 2048, /*gpuLayers*/ -1);
    worker.start();
    QMetaObject::invokeMethod(m_backend, &LlamaBackend::loadModel,
                              Qt::QueuedConnection);

    QVERIFY2(QTest::qWaitFor([&]() {
                 return loadedSpy.count() + failedSpy.count() > 0;
             },
                             120000),
             "Загрузка модели не завершилась за 120 с");
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(loadedSpy.count(), 1);
    QVERIFY(m_backend->isModelLoaded());
    // Загрузка — только в worker-потоке (не в потоке теста/UI).
    QCOMPARE(loadedThread, &worker);

    // --- Генерация: вызов из потока теста (как из UI) ------------------
    QThread* readyThread = nullptr;
    QObject::connect(m_backend, &ITextGenerationBackend::generationReady,
                     m_backend,
                     [&readyThread](const GenerationResult&) {
                         readyThread = QThread::currentThread();
                     },
                     Qt::DirectConnection);
    QSignalSpy readySpy(m_backend, &ITextGenerationBackend::generationReady);
    QSignalSpy errorSpy(m_backend, &ITextGenerationBackend::generationError);

    GenerationRequest request;
    request.requestId = 77;
    request.context = QStringLiteral("def calculate_sum(a, b):\n    ");
    request.maxTokens = 32;
    m_backend->generate(request);

    QVERIFY2(QTest::qWaitFor([&]() {
                 return readySpy.count() + errorSpy.count() > 0;
             },
                             60000),
             "Генерация не завершилась за 60 с");
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(readySpy.count(), 1);

    const GenerationResult result =
        qvariant_cast<GenerationResult>(readySpy.first().at(0));
    QCOMPARE(result.requestId, quint64(77));
    QVERIFY(result.stopReason == QStringLiteral("stop")
            || result.stopReason == QStringLiteral("length"));
    QVERIFY(!result.generatedText.isEmpty());
    QVERIFY(result.elapsedMs > 0);
    // Инференс (llama_decode/sampling) — в worker-потоке, не в UI.
    QCOMPARE(readyThread, &worker);

    // Диагностика: метрики последнего успешного запроса заполнены —
    // ровно то, что показывает режим Diagnostics (после отмены ниже
    // снимок обновится метриками отменённого запроса).
    const BackendDiagnostics diag = m_backend->diagnostics();
    QCOMPARE(diag.backendName, QStringLiteral("llama.cpp"));
    QCOMPARE(diag.modelName, QFileInfo(modelPath).fileName());
    QCOMPARE(diag.contextSize, 2048);
    QVERIFY2(diag.promptTokens > 0,
             "Prompt tokens не записаны в снимок");
    QVERIFY2(diag.generatedTokens > 0,
             "Generated tokens не записаны в снимок");
    QVERIFY2(diag.promptProcessingMs > 0,
             "Время обработки prompt'а не записано в снимок");
    QVERIFY2(diag.generationMs > 0,
             "Время генерации не записано в снимок");
    QVERIFY2(diag.lastError.isEmpty(),
             qPrintable(diag.lastError));

    // Модель загружается один раз: повторный loadModel() не перегружает
    // (elapsedMs == 0 в сигнале), isModelLoaded остаётся true.
    QSignalSpy reloadSpy(m_backend, &LlamaBackend::modelLoaded);
    QMetaObject::invokeMethod(m_backend, &LlamaBackend::loadModel,
                              Qt::QueuedConnection);
    QVERIFY(QTest::qWaitFor([&]() { return reloadSpy.count() > 0; }, 5000));
    QCOMPARE(reloadSpy.first().at(2).toLongLong(), qint64(0));
    QVERIFY(m_backend->isModelLoaded());

    // --- Отмена: атомарный флаг из любого потока -----------------------
    QSignalSpy cancelSpy(m_backend,
                         &ITextGenerationBackend::generationReady);
    GenerationRequest longRequest;
    longRequest.requestId = 78;
    longRequest.context = QStringLiteral("Далее текст продолжается: ");
    longRequest.maxTokens = 512;
    m_backend->generate(longRequest);
    m_backend->cancel(78); // до старта или внутри цикла — оба случая OK

    QVERIFY2(QTest::qWaitFor([&]() { return cancelSpy.count() > 0; },
                             60000),
             "Отменённый запрос не вернулся");
    const GenerationResult cancelled =
        qvariant_cast<GenerationResult>(cancelSpy.first().at(0));
    QCOMPARE(cancelled.requestId, quint64(78));
    QCOMPARE(cancelled.stopReason, QStringLiteral("cancelled"));
}

QTEST_GUILESS_MAIN(LlamaBackendTest)

#include "llama_backend_test.moc"
