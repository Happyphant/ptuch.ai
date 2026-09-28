// editor_adapter.cpp
#include "editor_adapter.h"

#include <QPlainTextEdit>
#include <QTextDocument>

#include <cassert>

PlainTextEditorAdapter::PlainTextEditorAdapter(QPlainTextEdit* editor,
                                               QObject* parent)
    : QObject(parent)
    , m_editor(editor)
{
    assert(m_editor != nullptr);
}

QTextCursor PlainTextEditorAdapter::textCursor() const
{
    return m_editor->textCursor();
}

QString PlainTextEditorAdapter::documentText() const
{
    return m_editor->toPlainText();
}

quint64 PlainTextEditorAdapter::documentRevision() const
{
    return static_cast<quint64>(m_editor->document()->revision());
}

void PlainTextEditorAdapter::insertText(const QString& text)
{
    QTextCursor cursor = m_editor->textCursor();
    cursor.insertText(text);
    m_editor->setTextCursor(cursor);
}

bool PlainTextEditorAdapter::hasFocus() const
{
    return m_editor->hasFocus();
}

bool PlainTextEditorAdapter::isReadOnly() const
{
    return m_editor->isReadOnly();
}
