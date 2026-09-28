// document_state.cpp
#include "document_state.h"

namespace {

// Не рвём суррогатную пару UTF-16 на границе вырезки.
int adjustNotInsideSurrogate(const QString& text, int position)
{
    if (position > 0 &&
        position < text.size() &&
        text.at(position).isLowSurrogate()) {
        return position + 1;
    }
    return position;
}

} // namespace

int DocumentState::sanitizeLimit(int requestedChars)
{
    if (requestedChars <= 0)
        return kDefaultContextLimitChars;

    return qMin(requestedChars, kHardContextLimitChars);
}

DocumentState DocumentState::capture(const QString& text,
                                     int cursorPosition,
                                     const QString& selectedText,
                                     quint64 generationId,
                                     qint64 lastModifiedMs,
                                     int contextLimitChars)
{
    DocumentState state;
    state.m_text = text;
    state.m_cursorPosition =
        qBound(0, cursorPosition, static_cast<int>(text.size()));
    state.m_selection = selectedText;
    state.m_generationId = generationId;
    state.m_lastModifiedMs = lastModifiedMs;
    state.m_contextLimitChars = sanitizeLimit(contextLimitChars);
    state.m_prefix = state.prefixFor(state.m_contextLimitChars);
    return state;
}

QString DocumentState::prefixFor(int limitChars) const
{
    if (m_cursorPosition <= 0 || m_text.isEmpty())
        return {};

    const int start =
        qMax(0, m_cursorPosition - limitChars);
    const int safeStart =
        adjustNotInsideSurrogate(m_text, start);

    return m_text.mid(safeStart, m_cursorPosition - safeStart);
}

DocumentContext DocumentState::buildContext(int maxPrefixChars) const
{
    DocumentContext ctx;

    const int prefixLimit = sanitizeLimit(maxPrefixChars);
    const bool customPrefixLimit =
        maxPrefixChars > 0 && maxPrefixChars < m_contextLimitChars;

    ctx.generationId = m_generationId;
    ctx.cursorPosition = m_cursorPosition;

    // Префикс: последние prefixLimit символов перед курсором.
    ctx.prefix = prefixFor(prefixLimit);
    ctx.prefixTruncated =
        m_cursorPosition > ctx.prefix.size();

    // Суффикс: ограничиваем и его — весь хвост документа тоже не шлём.
    const int suffixLimit =
        customPrefixLimit ? prefixLimit : m_contextLimitChars;
    const int suffixStart = m_cursorPosition;
    const int suffixEnd = qMin(
        static_cast<int>(m_text.size()), suffixStart + suffixLimit);

    ctx.suffix = m_text.mid(suffixStart, suffixEnd - suffixStart);
    ctx.suffixTruncated = suffixEnd < m_text.size();

    // Выделение — тем же жёстким потолком.
    ctx.selection = m_selection.left(kHardContextLimitChars);

    return ctx;
}
