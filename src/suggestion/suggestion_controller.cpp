// suggestion_controller.cpp
#include "suggestion_controller.h"

//#include <QKeyEvent>
#include <QTextDocument>

#include <algorithm>
#include <functional>

SuggestionController::SuggestionController(
    ISuggestionEditor* editor,
    ISuggestionBackend* backend,
    QObject* parent)
    : QObject(parent)
    , m_editor(editor)
    , m_backend(backend)
{
    Q_ASSERT(m_editor != nullptr);
    Q_ASSERT(m_backend != nullptr);

    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(500);

    connect(
        &m_debounceTimer,
        &QTimer::timeout,
        this,
        &SuggestionController::startRequest);
}

void SuggestionController::setStyles(QVector<StyleWeight> styles)
{
    m_styles = std::move(styles);

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
    }
}

void SuggestionController::onTextChanged()
{
    clearSuggestion();

    if (!m_enabled)
        return;

    scheduleRequest();
}

void SuggestionController::onCursorPositionChanged()
{
    clearSuggestion();

    if (!m_enabled)
        return;

    scheduleRequest();
}

void SuggestionController::scheduleRequest()
{
    m_debounceTimer.stop();

    if (!m_editor->hasFocus())
        return;

    if (m_editor->isReadOnly())
        return;

    const QTextCursor cursor = m_editor->textCursor();

    if (!cursor.hasSelection() && cursor.position() == 0)
        return;

    m_debounceTimer.start();
}

void SuggestionController::cancelCurrentRequest()
{
    if (m_cancelToken)
        m_cancelToken->store(true, std::memory_order_relaxed);

    m_cancelToken.reset();
    m_requestInProgress = false;
    ++m_requestId;
}

SuggestionContext SuggestionController::makeContext() const
{
    SuggestionContext context;

    context.cursor = m_editor->textCursor();
    context.documentRevision = m_editor->documentRevision();
    context.styles = m_styles;
    context.alternativeIndex = m_alternativeIndex;

    const QString document = m_editor->documentText();
    const int position = context.cursor.position();

    context.textBeforeCursor = document.left(position);
    context.textAfterCursor = document.mid(position);

    return context;
}

void SuggestionController::startRequest()
{
    if (!m_enabled ||
        !m_editor->hasFocus() ||
        m_editor->isReadOnly()) {
        return;
    }

    cancelCurrentRequest();

    const SuggestionContext context = makeContext();

    if (context.textBeforeCursor.trimmed().isEmpty())
        return;

    const quint64 requestId = ++m_requestId;

    m_documentRevision = context.documentRevision;
    m_cursorPosition = context.cursor.position();
    m_requestInProgress = true;

    m_cancelToken =
        std::make_shared<std::atomic_bool>(false);

    emit requestStarted();

    m_backend->requestSuggestion(
        context,
        m_cancelToken,
        [this, requestId](SuggestionResult result) {
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result = std::move(result)]() mutable {
                    if (requestId != m_requestId)
                        return;

                    m_requestInProgress = false;
                    m_cancelToken.reset();

                    emit requestFinished();

                    if (!isResultValid(result))
                        return;

                    setSuggestion(result);
                },
                Qt::QueuedConnection);
        });
}

bool SuggestionController::isResultValid(
    const SuggestionResult& result) const
{
    if (!m_enabled)
        return false;

    if (!m_editor->hasFocus())
        return false;

    if (m_editor->isReadOnly())
        return false;

    if (result.requestId != 0 &&
        result.requestId != m_requestId) {
        return false;
    }

    if (result.documentRevision != m_editor->documentRevision())
        return false;

    if (result.position != m_editor->textCursor().position())
        return false;

    if (result.text.trimmed().isEmpty())
        return false;

    return true;
}

void SuggestionController::setSuggestion(
    const SuggestionResult& result)
{
    m_suggestion = result.text;
    m_documentRevision = result.documentRevision;
    m_cursorPosition = result.position;
    m_alternativeIndex = result.alternativeIndex;

    emit suggestionChanged(m_suggestion);
}

void SuggestionController::clearSuggestion()
{
    if (m_suggestion.isEmpty())
        return;

    m_suggestion.clear();
    emit suggestionCleared();
}

QString SuggestionController::suggestion() const
{
    return m_suggestion;
}

bool SuggestionController::hasSuggestion() const
{
    return !m_suggestion.isEmpty();
}

void SuggestionController::requestAlternative()
{
    clearSuggestion();

    ++m_alternativeIndex;

    if (!m_enabled)
        return;

    cancelCurrentRequest();
    startRequest();
}

bool SuggestionController::handleKeyPress(QKeyEvent* event)
{
    if (!event || !m_enabled)
        return false;

    if (event->key() == Qt::Key_Tab &&
        event->modifiers() == Qt::NoModifier &&
        hasSuggestion()) {
        const QString acceptedText = m_suggestion;

        m_editor->insertText(acceptedText);

        clearSuggestion();
        m_alternativeIndex = 0;

        emit suggestionAccepted(acceptedText);

        event->accept();
        return true;
    }

    if (event->key() == Qt::Key_Tab &&
        event->modifiers() == Qt::ShiftModifier) {
        requestAlternative();

        event->accept();
        return true;
    }

    if (event->key() == Qt::Key_Escape &&
        hasSuggestion()) {
        clearSuggestion();

        event->accept();
        return true;
    }

    return false;
}
