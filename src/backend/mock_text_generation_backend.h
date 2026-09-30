// mock_text_generation_backend.h
#pragma once

#include "text_generation_backend.h"

#include <QSet>

// Mock AI-backend'а: тестовая подсказка через небольшую задержку,
// без модели. НЕ живёт в UI-потоке: создатель обязан сделать
// moveToThread() в рабочий QThread (так и используется в MainWindow
// и в тестах).
//
// generate()/cancel() безопасны из любого потока: вызов лишь
// ставит работу в очередь события потока объекта.
//
// Режимы для тестов (детерминизм: задержка / результат / ошибка):
//  - setDelayMs()        — задержка ответа, мс (тайминги без сна в тесте);
//  - setResultText()     — ровно заданный текст ответа (иначе —
//                          автоподсказка по последнему слову контекста);
//  - setSimulateError()  — имитация ошибки генерации;
//  - setIgnoreCancel()   — «плохой» backend: игнорирует cancel()
//                          и отвечает на отменённый запрос (проверка,
//                          что потребитель отбрасывает по requestId).
class MockTextGenerationBackend final : public ITextGenerationBackend
{
    Q_OBJECT

public:
    explicit MockTextGenerationBackend(QObject* parent = nullptr);

    // Настройки вызываются ДО запуска потока (не потокобезопасны).
    void setDelayMs(int delayMs);
    void setSimulateError(bool enabled);
    // Детерминированный результат: generate() вернёт ровно этот текст.
    // Пустая строка (дефолт) — как раньше, автоподсказка по контексту.
    void setResultText(const QString& text);
    void setIgnoreCancel(bool enabled);

    // ITextGenerationBackend
    void generate(const GenerationRequest& request) override;
    void cancel(quint64 requestId) override;
    // Диагностика: константный снимок («mock», модель и метрики
    // отсутствуют — UI показывает «—»). Потокобезопасен: разделяемого
    // состояния нет.
    BackendDiagnostics diagnostics() const override;

private:
    // Ниже — только в потоке объекта.
    void scheduleFinish(GenerationRequest request);
    void finish(GenerationRequest request,
                bool cancelled,
                qint64 elapsedMs);

    int m_delayMs = 300;
    bool m_simulateError = false;
    bool m_ignoreCancel = false;
    QString m_resultText; // пусто = автоподсказка по контексту

    QSet<quint64> m_cancelledRequests;
};
