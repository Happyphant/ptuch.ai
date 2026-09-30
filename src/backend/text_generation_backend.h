// text_generation_backend.h
#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

// Стиль подсказки: backend может учесть веса при построении промпта.
struct StyleWeight
{
    QString styleId;
    float weight = 0.0f;
};

// Описание адаптера в запросе — нейтральный контракт на Qt-типах
// (QString/double), без привязки к библиотеке обучения. Пустой path =
// адаптер не запрошен (текущий MVP — всегда пусто; см. README,
// раздел «Стили и LoRA-адаптеры»).
struct AdapterSpec
{
    QString path;        // файл адаптера; пусто = нет адаптера
    QString type;        // свободный тег формата: "lora", "dora", ...
    QString baseModelId; // базовая модель, под которую обучен адаптер
    QString promptTag;   // триггер-метка (для adapter-capable backend'ов)
    double scale = 1.0;  // сила применения; интерпретация — за backend'ом

    bool isEmpty() const { return path.isEmpty(); }
};

// Запрос на асинхронную генерацию текста.
struct GenerationRequest
{
    // Явный id: backend обязан вернуть его в результате — потребитель
    // по нему отличает актуальный ответ от устаревшего.
    quint64 requestId = 0;
    // Обрезанный контекст (никогда — весь документ целиком).
    QString context;
    QString systemPrompt;
    int maxTokens = 64;
    double temperature = 0.7;
    double topP = 0.9;
    QVector<StyleWeight> styleWeights;
    // Опциональный адаптер (пусто в MVP). Backend без поддержки
    // (supportsAdapters() == false) ОБЯЗАН ответить generationError с
    // явным объяснением — не игнорировать молча (см. mock/llama
    // generate()).
    AdapterSpec adapter;
};

struct GenerationResult
{
    // Эхо GenerationRequest::requestId.
    quint64 requestId = 0;
    QString generatedText;
    // Причина остановки: "stop" | "length" | "cancelled" | ...
    QString stopReason;
    // Сколько заняла генерация (мс).
    qint64 elapsedMs = 0;
};

// Снимок диагностики backend'а (режим диагностики в UI): состояние
// последнего успешного запроса + последняя ошибка.
//
// Только доменные значения — без указателей, адресов и внутренних
// структур backend'а: отображаемый/копируемый текст форматируется
// строго из этих полей (см. DiagnosticsDialog::formatReport).
struct BackendDiagnostics
{
    QString backendName;        // "llama.cpp" | "mock" (пусто = н/д)
    QString modelName;          // имя файла GGUF; пусто = модели нет
    int contextSize = 0;        // n_ctx (настроенный/фактический)
    int gpuLayers = 0;          // -1 = все слои на GPU
    qint64 promptTokens = 0;    // токены последнего успешного prompt'а
    qint64 generatedTokens = 0; // сгенерировано последним запросом
    qint64 promptProcessingMs = 0; // время обработки prompt'а (мс)
    qint64 generationMs = 0;    // время генерации (мс)
    QString lastError;          // последняя ошибка (пусто = нет)
};

Q_DECLARE_METATYPE(GenerationRequest)
Q_DECLARE_METATYPE(GenerationResult)
Q_DECLARE_METATYPE(BackendDiagnostics)

// Абстракция AI-backend'а генерации текста.
//
// Dependency injection: потребитель (SuggestionController) знает только
// этот интерфейс; mock и будущий llama-backend подставляются сверху
// через setBackend() без изменения потребителя.
//
// Потоки: реализация МОЖЕТ жить вне UI-потока. generate()/cancel()
// разрешено вызывать из любого потока — реализация сама переносит
// работу в свой поток. Результат приходит сигналами generationReady /
// generationError из потока объекта; потребитель в другом потоке
// получает их автоматически queued (Qt).
//
// Отмена — «вежливость»: реализация вправе её игнорировать и всё равно
// ответить, поэтому потребитель ОБЯЗАН сверять requestId результата.
class ITextGenerationBackend : public QObject
{
    Q_OBJECT

public:
    explicit ITextGenerationBackend(QObject* parent = nullptr)
        : QObject(parent)
    {
        // Типы параметров сигналов — для queued-доставки между потоками.
        qRegisterMetaType<GenerationRequest>("GenerationRequest");
        qRegisterMetaType<GenerationResult>("GenerationResult");
    }

    ~ITextGenerationBackend() override = default;

    // Асинхронно: возвращает управление сразу, не блокирует вызывающего.
    // Контракт адаптеров: при непустом request.adapter и
    // supportsAdapters() == false реализация обязана вернуть
    // generationError с явным текстом «не поддерживает адаптеры», а не
    // тихо игнорировать (никакой фиктивной загрузки LoRA).
    virtual void generate(const GenerationRequest& request) = 0;

    // Поддержка внешних адаптеров (LoRA/…). false по умолчанию: mock и
    // текущий GGUF-backend работают без адаптеров — смешивание стилей
    // идёт только через prompt. Настоящее взвешенное наложение LoRA
    // требует отдельного inference backend'а, который переопределит
    // этот метод (и сам загрузит adapterPath).
    virtual bool supportsAdapters() const { return false; }

    // Просьба прекратить генерацию requestId (может быть проигнорирована
    // — см. договорённость про сверку requestId выше).
    virtual void cancel(quint64 requestId) = 0;

    // Режим диагностики: независимый снимок состояния backend'а.
    // Разрешён из любого потока (вызывается из UI): реализация обязана
    // вернуть КОПИЮ под своим синхронизирующим объектом — короткая
    // блокировка, без обращения к внутренним указателям и без
    // длительного удержания (UI не блокируется).
    virtual BackendDiagnostics diagnostics() const = 0;

signals:
    void generationReady(const GenerationResult& result);
    void generationError(quint64 requestId, const QString& message);
};
