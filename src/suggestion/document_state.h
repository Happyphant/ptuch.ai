// document_state.h
#pragma once

#include <QString>
#include <QtGlobal>

// Результат безопасного построения контекста для автодополнения.
// Все строки здесь ограничены по размеру: полный документ в модель не уходит.
struct DocumentContext
{
    QString prefix;    // последние символы перед курсором (обрезано)
    QString suffix;    // символы после курсора (обрезаны)
    QString selection; // выделенный текст (обрезан)

    int cursorPosition = 0;

    bool prefixTruncated = false;
    bool suffixTruncated = false;

    quint64 generationId = 0;
};

// Снимок состояния документа для построения контекста автодополнения.
//
// Гарантии:
//  - хранит полный текст, позицию курсора, выделение, последние N символов
//    перед курсором, timestamp изменения и generation id запроса;
//  - buildContext() НИКОГДА не возвращает весь документ: контекст обрезается
//    по символам, жёсткий потолок — kHardContextLimitChars;
//  - ghost-подсказка не является частью состояния и в контекст не попадает
//    (снимок строится только из текста документа);
//  - generation id позволяет игнорировать устаревшие ответы
//    (isGenerationCurrent()).
//
// Объект value-типа: глобального mutable-состояния нет.
class DocumentState
{
public:
    // Размер контекста по умолчанию (N символов перед курсором).
    static constexpr int kDefaultContextLimitChars = 2000;
    // Жёсткий потолок: больше в контекст не попадёт ни при каких аргументах.
    static constexpr int kHardContextLimitChars = 8000;

    DocumentState() = default;

    // Снимок документа. cursorPosition зажимается в границы [0, text.size()].
    // lastModifiedMs — метка времени последнего изменения (мс с эпохи).
    static DocumentState capture(const QString& text,
                                 int cursorPosition,
                                 const QString& selectedText,
                                 quint64 generationId,
                                 qint64 lastModifiedMs,
                                 int contextLimitChars = kDefaultContextLimitChars);

    const QString& text() const { return m_text; }
    int cursorPosition() const { return m_cursorPosition; }
    const QString& selectedText() const { return m_selection; }
    // Последние N символов перед курсором (N = contextLimitChars()).
    const QString& recentTextBeforeCursor() const { return m_prefix; }
    qint64 lastModifiedMs() const { return m_lastModifiedMs; }
    quint64 generationId() const { return m_generationId; }
    int contextLimitChars() const { return m_contextLimitChars; }

    // true, если ответ с таким generation id не устарел.
    bool isGenerationCurrent(quint64 responseGenerationId) const
    {
        return responseGenerationId == m_generationId;
    }

    // Делает все ранее выданные (в полёте) ответы устаревшими.
    void setGenerationId(quint64 generationId)
    {
        m_generationId = generationId;
    }

    // Безопасный контекст для автодополнения.
    // maxPrefixChars <= 0 — использовать contextLimitChars();
    // иначе значение зажимается в kHardContextLimitChars.
    DocumentContext buildContext(int maxPrefixChars = 0) const;

private:
    static int sanitizeLimit(int requestedChars);

    // Общая логика вычисления префикса: [cursor - limit, cursor),
    // не режет суррогатные пары.
    QString prefixFor(int limitChars) const;

    QString m_text;
    QString m_selection;
    QString m_prefix;
    int m_cursorPosition = 0;
    qint64 m_lastModifiedMs = 0;
    quint64 m_generationId = 0;
    int m_contextLimitChars = kDefaultContextLimitChars;
};
