// suggestion_controller.cpp
#include "suggestion_controller.h"

#include <QDateTime>

#include <utility>

SuggestionController::SuggestionController(ISuggestionEditor* editor,
                                           QObject* parent)
    : QObject(parent)
    , m_editor(editor)
{
    Q_ASSERT(m_editor != nullptr);

    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(500); // debounce печати, мс

    connect(&m_debounceTimer, &QTimer::timeout, this, [this]() {
        // Автозапуск требует фокуса. Если контекст стал непригодным —
        // просто возвращаемся в Idle (не ошибка, генерация не нужна).
        QString error;
        if (!startRequest(/*requireFocus*/ true, &error))
            setState(State::Idle);
    });
}

void SuggestionController::setBackend(ITextGenerationBackend* backend)
{
    if (m_backend == backend)
        return;

    // Отмена уходит на прежний backend; bump generation id делает
    // его будущие ответы устаревшими.
    cancelCurrentRequest();

    if (m_backend)
        disconnect(m_backend, nullptr, this, nullptr);

    m_backend = backend;

    if (m_backend != nullptr) {
        // Backend может жить в другом потоке — доставка будет queued,
        // обработка ответа идёт в потоке контроллера.
        connect(m_backend, &ITextGenerationBackend::generationReady,
                this, &SuggestionController::onGenerationReady);
        connect(m_backend, &ITextGenerationBackend::generationError,
                this, &SuggestionController::onGenerationError);
    }

    if (m_state == State::Generating)
        setState(State::Idle);
}

void SuggestionController::setGenerationParams(const QString& systemPrompt,
                                               int maxTokens,
                                               double temperature,
                                               double topP)
{
    m_systemPrompt = systemPrompt;
    m_maxTokens = maxTokens;
    m_temperature = temperature;
    m_topP = topP;
}

void SuggestionController::setStyleMix(QVector<StyleWeight> mix)
{
    m_styles = std::move(mix);

    // Показанная подсказка сделана под прежний mix — убираем
    // и перепланируем запрос.
    clearSuggestion();
    scheduleRequest();
}

void SuggestionController::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;

    m_enabled = enabled;

    if (!m_enabled) {
        m_debounceTimer.stop();
        cancelCurrentRequest();
        clearSuggestion();
        setState(State::Idle);
    }
}

void SuggestionController::shutdown()
{
    // Вызывается из MainWindow::closeEvent: debounce и ответы backend'а
    // не должны обрабатываться после начала разрушения виджетов.
    // Контроллер отвязывается от backend'а; опоздавшие ответы
    // отсекаются несовпадением generation id.
    setEnabled(false);
    setBackend(nullptr);
}

void SuggestionController::onTextChanged()
{
    if (!m_enabled)
        return;

    // Пользователь продолжает печатать:
    //  - активный запрос устаревает (его результат будет отброшен);
    //  - показанная подсказка невалидна;
    //  - debounce запускается заново — новая генерация НЕ стартует,
    //    пока печать не остановится на 500 мс.
    cancelCurrentRequest();
    clearSuggestion();
    scheduleRequest();
}

void SuggestionController::onCursorPositionChanged()
{
    if (!m_enabled)
        return;

    // Смена позиции так же обесценивает запрос и показанную подсказку.
    cancelCurrentRequest();
    clearSuggestion();
    scheduleRequest();
}

void SuggestionController::scheduleRequest()
{
    m_debounceTimer.stop();

    if (!m_enabled ||
        !m_editor->hasFocus() ||
        m_editor->isReadOnly()) {
        setState(State::Idle);
        return;
    }

    const QTextCursor cursor = m_editor->textCursor();

    if (!cursor.hasSelection() && cursor.position() == 0) {
        // Нечего генерировать: до курсора ничего нет.
        setState(State::Idle);
        return;
    }

    m_debounceTimer.start();
    setState(State::Debouncing);
}

void SuggestionController::cancelCurrentRequest()
{
    // Активного запроса нет — generation id не трогаем.
    if (!m_requestInProgress)
        return;

    // Два уровня защиты:
    //  1) вежливая отмена — cancel(requestId) к backend'у;
    //  2) bump generation id — если backend проигнорирует отмену,
    //     его ответ всё равно не пройдёт сверку requestId.
    if (m_backend != nullptr)
        m_backend->cancel(m_generationId);

    m_requestInProgress = false;
    m_focusRequiredForCurrentRequest = true;

    ++m_generationId;
    // Делаем снимок документа устаревшим: прежние generation id
    // перестанут проходить isGenerationCurrent().
    m_documentState.setGenerationId(m_generationId);
}

SuggestionContext SuggestionController::makeContext()
{
    SuggestionContext context;

    const QTextCursor cursor = m_editor->textCursor();
    const QString document = m_editor->documentText();
    const int position = cursor.position();

    const int selectionStart = cursor.selectionStart();
    const int selectionEnd = cursor.selectionEnd();
    const QString selection =
        document.mid(selectionStart, selectionEnd - selectionStart);

    // Ghost-подсказка намеренно НЕ передаётся: снимок строится только из
    // текста документа (m_suggestion в документ не вставляется).
    m_documentState = DocumentState::capture(
        document,
        position,
        selection,
        m_generationId,                        // generation id запроса
        QDateTime::currentMSecsSinceEpoch());  // timestamp изменения

    const DocumentContext docContext = m_documentState.buildContext();

    context.cursor = cursor;
    context.documentRevision = m_editor->documentRevision();
    context.styles = m_styles;
    context.alternativeIndex = m_alternativeIndex;
    context.generationId = docContext.generationId;

    // Безопасный контекст: префикс/суффикс обрезаны по символам,
    // весь документ в модель не уходит.
    context.textBeforeCursor = docContext.prefix;
    context.textAfterCursor = docContext.suffix;

    return context;
}

bool SuggestionController::startRequest(bool requireFocus,
                                        QString* errorMessage)
{
    const auto fail = [errorMessage](const QString& message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };

    if (!m_enabled)
        return fail(tr("Контроллер отключён"));
    if (m_backend == nullptr)
        return fail(tr("Backend не задан"));
    if (m_editor->isReadOnly())
        return fail(tr("Редактор только для чтения"));
    if (requireFocus && !m_editor->hasFocus())
        return fail(tr("Редактор не в фокусе"));

    // Прежний запрос (если был «в полёте») отменяется/помечается
    // устаревшим.
    cancelCurrentRequest();

    // Каждый новый запрос получает новый generation id:
    // принят будет только ответ с текущим id.
    ++m_generationId;

    const SuggestionContext context = makeContext();

    if (context.textBeforeCursor.trimmed().isEmpty())
        return fail(tr("Пустой контекст для генерации"));

    m_documentRevision = context.documentRevision;
    m_cursorPosition = context.cursor.position();
    m_requestInProgress = true;
    m_focusRequiredForCurrentRequest = requireFocus;

    // Показанная ранее подсказка при новом запросе устаревает.
    clearSuggestion();

    emit suggestionStarted();
    setState(State::Generating);

    GenerationRequest request;
    request.requestId = m_generationId;
    // В backend уходит только обрезанный префикс; суффикс и
    // alternativeIndex в GenerationRequest пока не входят.
    request.context = context.textBeforeCursor;
    request.systemPrompt = m_systemPrompt;
    request.maxTokens = m_maxTokens;
    request.temperature = m_temperature;
    request.topP = m_topP;
    request.styleWeights = context.styles;

    // Асинхронно: вызов лишь ставит работу в очередь backend'а,
    // UI не блокируется.
    m_backend->generate(request);

    return true;
}

void SuggestionController::onGenerationReady(const GenerationResult& result)
{
    // Устаревший ответ: был новый запрос, отмена или смена backend'а —
    // в любом случае generation id уже не совпадает.
    if (!m_enabled ||
        !m_requestInProgress ||
        result.requestId != m_generationId)
        return;

    m_requestInProgress = false;

    if (!isResultValid(result)) {
        emit suggestionFailed(
            tr("Результат отклонён: документ изменился или он пуст"),
            result.requestId);
        setState(State::Error);
        return;
    }

    m_suggestion = result.generatedText;
    emit suggestionReady(result.generatedText, result.requestId);
    setState(State::Ready);
}

void SuggestionController::onGenerationError(quint64 requestId,
                                             const QString& message)
{
    // Ошибка устаревшего запроса никого не интересует.
    if (!m_enabled ||
        !m_requestInProgress ||
        requestId != m_generationId)
        return;

    m_requestInProgress = false;

    emit suggestionFailed(message, requestId);
    setState(State::Error);
}

bool SuggestionController::isResultValid(
    const GenerationResult& result) const
{
    if (!m_enabled)
        return false;

    // Ручной запуск (Generate) допускает потерю фокуса между нажатием
    // кнопки и приходом результата.
    if (m_focusRequiredForCurrentRequest && !m_editor->hasFocus())
        return false;

    if (m_editor->isReadOnly())
        return false;

    // Снимки на момент диспетчеризации: документ и курсор не должны
    // быть изменены, пока backend генерировал.
    if (m_documentRevision != m_editor->documentRevision())
        return false;

    if (m_cursorPosition != m_editor->textCursor().position())
        return false;

    if (result.generatedText.trimmed().isEmpty())
        return false;

    return true;
}

void SuggestionController::requestSuggestion()
{
    // Ручной запуск минует debounce и не требует фокуса.
    m_debounceTimer.stop();

    QString error;
    if (!startRequest(/*requireFocus*/ false, &error)) {
        // Ручной запуск не может «пройти мимо»: ошибка видима,
        // состояние уходит в Error (не зависает в Debouncing).
        emit suggestionFailed(error, m_generationId);
        setState(State::Error);
    }
}

void SuggestionController::acceptSuggestion()
{
    if (m_suggestion.isEmpty())
        return;

    const QString text = m_suggestion;
    m_suggestion.clear();
    m_alternativeIndex = 0;

    // Вставка идёт через интерфейс редактора (не виджет): сигнал
    // textChanged синхронно вызовет onTextChanged() — там подсказка
    // уже пуста, дублей suggestionCleared не будет.
    m_editor->insertText(text);

    emit suggestionCleared();
}

void SuggestionController::rejectSuggestion()
{
    m_debounceTimer.stop();

    const bool hadSuggestion = !m_suggestion.isEmpty();

    // Активный запрос отменяется (cancel + bump id): его результат
    // «не воскресит» подсказку, даже если backend игнорирует отмену.
    cancelCurrentRequest();

    if (hadSuggestion) {
        m_suggestion.clear();
        emit suggestionCleared();
    }

    m_alternativeIndex = 0;
    setState(State::Idle);
}

void SuggestionController::requestAlternative()
{
    if (!m_enabled)
        return;

    m_debounceTimer.stop();
    // Прежняя подсказка устаревает даже если новый запрос не стартует.
    clearSuggestion();
    ++m_alternativeIndex;

    QString error;
    if (!startRequest(/*requireFocus*/ true, &error))
        setState(State::Idle);
}

QString SuggestionController::suggestion() const
{
    return m_suggestion;
}

bool SuggestionController::hasSuggestion() const
{
    return !m_suggestion.isEmpty();
}

void SuggestionController::clearSuggestion()
{
    if (m_suggestion.isEmpty())
        return;

    m_suggestion.clear();
    emit suggestionCleared();
}

void SuggestionController::setState(State state)
{
    if (m_state == state)
        return;

    m_state = state;
    emit stateChanged(state);
}
