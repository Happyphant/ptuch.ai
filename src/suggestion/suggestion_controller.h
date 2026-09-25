// suggestion_controller.h
#pragma once

#include <QKeyEvent>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTextCursor>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <memory>

struct StyleWeight
{
    QString styleId;
    float weight = 0.0f;
};

struct SuggestionContext
{
    QString textBeforeCursor;
    QString textAfterCursor;
    QTextCursor cursor;
    quint64 documentRevision = 0;
    QVector<StyleWeight> styles;
    int alternativeIndex = 0;
};

struct SuggestionResult
{
    QString text;
    quint64 requestId = 0;
    quint64 documentRevision = 0;
    int position = 0;
    int alternativeIndex = 0;
};

Q_DECLARE_METATYPE(SuggestionResult)

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

class ISuggestionBackend
{
public:
    using CancelToken = std::shared_ptr<std::atomic_bool>;

    virtual ~ISuggestionBackend() = default;

    virtual void requestSuggestion(
        SuggestionContext context,
        CancelToken cancelToken,
        std::function<void(SuggestionResult)> callback) = 0;
};

class SuggestionController final : public QObject
{
    Q_OBJECT

public:
    explicit SuggestionController(
        ISuggestionEditor* editor,
        ISuggestionBackend* backend,
        QObject* parent = nullptr);

    void setStyles(QVector<StyleWeight> styles);
    void setEnabled(bool enabled);

    bool handleKeyPress(QKeyEvent* event);

    QString suggestion() const;
    bool hasSuggestion() const;

public slots:
    void onTextChanged();
    void onCursorPositionChanged();
    void requestAlternative();

signals:
    void suggestionChanged(const QString& text);
    void suggestionAccepted(const QString& text);
    void suggestionCleared();
    void requestStarted();
    void requestFinished();
    void errorOccurred(const QString& message);

private:
    void scheduleRequest();
    void startRequest();
    void cancelCurrentRequest();

    void setSuggestion(const SuggestionResult& result);
    void clearSuggestion();

    bool isResultValid(const SuggestionResult& result) const;
    SuggestionContext makeContext() const;

private:
    QPointer<QObject> m_editorObject;
    ISuggestionEditor* m_editor = nullptr;
    ISuggestionBackend* m_backend = nullptr;

    QTimer m_debounceTimer;

    QVector<StyleWeight> m_styles;

    QString m_suggestion;
    quint64 m_requestId = 0;
    quint64 m_documentRevision = 0;
    int m_cursorPosition = 0;
    int m_alternativeIndex = 0;

    bool m_enabled = true;
    bool m_requestInProgress = false;

    ISuggestionBackend::CancelToken m_cancelToken;
};
