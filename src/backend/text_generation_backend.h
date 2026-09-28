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

Q_DECLARE_METATYPE(GenerationRequest)
Q_DECLARE_METATYPE(GenerationResult)

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
    virtual void generate(const GenerationRequest& request) = 0;

    // Просьба прекратить генерацию requestId (может быть проигнорирована
    // — см. договорённость про сверку requestId выше).
    virtual void cancel(quint64 requestId) = 0;

signals:
    void generationReady(const GenerationResult& result);
    void generationError(quint64 requestId, const QString& message);
};
