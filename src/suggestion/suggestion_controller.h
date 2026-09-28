// suggestion_controller.h
#pragma once

#include "backend/text_generation_backend.h"
#include "document_state.h"

#include <QObject>
#include <QString>
#include <QTextCursor>
#include <QTimer>
#include <QVector>

struct SuggestionContext
{
    QString textBeforeCursor;   // обрезано DocumentState (не весь документ)
    QString textAfterCursor;    // обрезано DocumentState (не весь документ)
    QTextCursor cursor;
    quint64 documentRevision = 0;
    QVector<StyleWeight> styles;
    int alternativeIndex = 0;
    // Generation id запроса: совпадает с GenerationRequest::requestId.
    quint64 generationId = 0;
};

class ISuggestionEditor
{
public:
    virtual ~ISuggestionEditor() = default;

    virtual QTextCursor textCursor() const = 0;
    virtual QString documentText() const = 0;
    virtual quint64 documentRevision() const = 0;

    virtual void insertText(const QString& text) = 0;

    virtual bool hasFocus() const = 0;
    virtual bool isReadOnly() const = 0;
};

// Оркестратор подсказок: debounce печати, запросы к AI-backend'у,
// generation id против устаревших ответов. НЕ знает о виджетах:
// работает только через интерфейсы ISuggestionEditor /
// ITextGenerationBackend (mock подставляется через setBackend() —
// dependency injection).
//
// Поток: контроллер живёт в UI-потоке; backend может жить в другом —
// результат приходит queued-сигналами и принимается в потоке
// контроллера.
//
// Жизненный цикл: вызов shutdown() из closeEvent() окна останавливает
// debounce, отменяет активный запрос и отвязывает backend до разрушения
// UI; опоздавшие ответы отсекаются несовпадением generation id.
class SuggestionController final : public QObject
{
    Q_OBJECT

public:
    // Состояние для индикатора UI (индикатор рисует MainWindow).
    enum class State {
        Idle,       // нет активности
        Debouncing, // печать: debounce-таймер работает (500 мс)
        Generating, // backend выполняет запрос
        Ready,      // подсказка показана
        Error       // последняя попытка провалилась
    };
    Q_ENUM(State)

    // editor не владеет; backend задаётся позже через setBackend().
    explicit SuggestionController(ISuggestionEditor* editor,
                                  QObject* parent = nullptr);
    ~SuggestionController() override = default;

    // Ручной немедленный запрос (кнопка Generate): минует debounce.
    // При непригодном контексте испускает suggestionFailed.
    void requestSuggestion();
    // Принять подсказку: вставить текст в документ через интерфейс
    // редактора. Испускает suggestionCleared.
    void acceptSuggestion();
    // Отклонить подсказку (Escape / Clear): отмена debounce и активного
    // запроса, очистка показанной подсказки.
    void rejectSuggestion();

    // Смесь стилей для backend (захватывается в контекст запроса).
    void setStyleMix(QVector<StyleWeight> mix);
    // DI-точка подмены: mock -> реальный backend (llama) без изменения
    // контроллера. Активный запрос при смене отменяется как устаревший.
    // Контроллер backend не владеет.
    void setBackend(ITextGenerationBackend* backend);
    // Параметры генерации, передаваемые в каждый запрос.
    void setGenerationParams(const QString& systemPrompt,
                             int maxTokens,
                             double temperature,
                             double topP);

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    // Закрытие окна: стоп debounce, отмена активного запроса,
    // отвязка backend. Повторный запуск невозможен (enabled = false).
    void shutdown();

    State state() const { return m_state; }
    QString suggestion() const;
    bool hasSuggestion() const;
    quint64 generationId() const { return m_generationId; }
    ITextGenerationBackend* backend() const { return m_backend; }
    // Снимок документа последнего запроса (для отладки/тестов).
    const DocumentState& documentState() const { return m_documentState; }

public slots:
    // Приём изменений документа/курсора (подключается сигналами редактора).
    void onTextChanged();
    void onCursorPositionChanged();
    // Следующая альтернатива (Shift+Tab).
    void requestAlternative();

signals:
    void suggestionStarted();
    void suggestionReady(const QString& text, quint64 generationId);
    void suggestionFailed(const QString& error, quint64 generationId);
    void suggestionCleared();
    void stateChanged(State state);

private:
    // Диспетчеризация запроса. false + текст ошибки = запрос не стартовал
    // (пустой контекст, нет backend, нет фокуса, редактор read-only...).
    bool startRequest(bool requireFocus, QString* errorMessage);
    // (Пере)запуск debounce-таймера; при непригодных условиях — Idle.
    void scheduleRequest();
    // Отмена активного запроса: cancel(id) к backend'у + bump generation
    // id — даже если backend проигнорирует cancel, ответ будет отброшен.
    void cancelCurrentRequest();
    // Убрать показанную подсказку (испускает suggestionCleared).
    void clearSuggestion();
    // Единая точка испускания stateChanged (только при изменении).
    void setState(State state);

    // Приём ответов backend'а (подключается в setBackend()).
    void onGenerationReady(const GenerationResult& result);
    void onGenerationError(quint64 requestId, const QString& message);

    // Снимок документа + правила приёмки ответа.
    bool isResultValid(const GenerationResult& result) const;
    // Снимок состояния документа + безопасный (обрезанный) контекст.
    SuggestionContext makeContext();

private:
    ISuggestionEditor* m_editor = nullptr;    // не владеет
    ITextGenerationBackend* m_backend = nullptr; // не владеет

    QTimer m_debounceTimer;

    QVector<StyleWeight> m_styles;

    DocumentState m_documentState;

    QString m_suggestion;
    quint64 m_generationId = 0;
    // Снимки на момент диспетчеризации — для проверки, что документ
    // и курсор не изменились, пока backend «генерировал».
    quint64 m_documentRevision = 0;
    int m_cursorPosition = 0;
    int m_alternativeIndex = 0;

    // Параметры каждого запроса (см. GenerationRequest).
    QString m_systemPrompt;
    int m_maxTokens = 64;
    double m_temperature = 0.7;
    double m_topP = 0.9;

    State m_state = State::Idle;
    bool m_enabled = true;
    bool m_requestInProgress = false;
    // Для ручного запуска результат принимается и без фокуса в редакторе.
    bool m_focusRequiredForCurrentRequest = true;
};
