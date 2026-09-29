// mock_text_generation_backend.cpp
#include "mock_text_generation_backend.h"

#include <QDateTime>
#include <QTimer>

#include <utility>

namespace {
// Фиктивная подсказка: продолжение последнего слова контекста.
QString makeTestSuggestion(const QString& context)
{
    int end = context.size();
    while (end > 0 && !context.at(end - 1).isSpace())
        --end;

    const QString lastWord = context.mid(end);

    return lastWord.isEmpty()
        ? QStringLiteral(" suggestion")
        : QStringLiteral(" continuation of \"%1\"").arg(lastWord);
}
} // namespace

MockTextGenerationBackend::MockTextGenerationBackend(QObject* parent)
    : ITextGenerationBackend(parent)
{
}

void MockTextGenerationBackend::setDelayMs(int delayMs)
{
    m_delayMs = delayMs;
}

void MockTextGenerationBackend::setSimulateError(bool enabled)
{
    m_simulateError = enabled;
}

void MockTextGenerationBackend::setIgnoreCancel(bool enabled)
{
    m_ignoreCancel = enabled;
}

void MockTextGenerationBackend::generate(const GenerationRequest& request)
{
    // Вызов может прийти из UI-потока: переносим работу в поток объекта.
    QMetaObject::invokeMethod(
        this,
        [this, request]() { scheduleFinish(request); },
        Qt::QueuedConnection);
}

void MockTextGenerationBackend::cancel(quint64 requestId)
{
    // Аналогично — только в поток объекта; никакой работы в UI-потоке.
    QMetaObject::invokeMethod(
        this,
        [this, requestId]() { m_cancelledRequests.insert(requestId); },
        Qt::QueuedConnection);
}

BackendDiagnostics MockTextGenerationBackend::diagnostics() const
{
    // Модели и настоящих токенов у mock нет: modelName/promptTokens/...
    // остаются нулевыми, и UI рисует для них «—» (см.
    // DiagnosticsDialog::formatReport). Состояние не разделяется —
    // вызов безопасен из любого потока без блокировок.
    BackendDiagnostics data;
    data.backendName = QStringLiteral("mock");
    return data;
}

void MockTextGenerationBackend::scheduleFinish(GenerationRequest request)
{
    const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();

    // singleShot с контекстом this привязан к жизненному циклу объекта:
    // после остановки потока колбэк не выполнится. UI не блокируется.
    QTimer::singleShot(
        m_delayMs, this,
        [this, request = std::move(request), startedAt]() {
            const bool cancelled =
                m_cancelledRequests.remove(request.requestId) > 0;
            const qint64 elapsedMs =
                QDateTime::currentMSecsSinceEpoch() - startedAt;
            finish(request, cancelled, elapsedMs);
        });
}

void MockTextGenerationBackend::finish(GenerationRequest request,
                                       bool cancelled,
                                       qint64 elapsedMs)
{
    // Отменённый запрос «игнорируется»: ответа нет — если, конечно,
    // backend не настроен игнорировать саму отмену.
    if (cancelled && !m_ignoreCancel)
        return;

    if (m_simulateError) {
        emit generationError(
            request.requestId,
            QStringLiteral("Mock: имитация ошибки генерации"));
        return;
    }

    GenerationResult result;
    result.requestId = request.requestId;
    result.generatedText = makeTestSuggestion(request.context);
    result.stopReason =
        cancelled ? QStringLiteral("cancelled") : QStringLiteral("stop");
    result.elapsedMs = elapsedMs;

    emit generationReady(result);
}
