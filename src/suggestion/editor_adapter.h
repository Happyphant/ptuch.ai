// editor_adapter.h
#pragma once

#include "suggestion_controller.h"

#include <QObject>

class QPlainTextEdit;

// Адаптер QPlainTextEdit к интерфейсу ISuggestionEditor.
// Владеет НЕ редактором (он принадлежит MainWindow), только ссылкой.
class PlainTextEditorAdapter final : public QObject, public ISuggestionEditor
{
    Q_OBJECT

public:
    PlainTextEditorAdapter(QPlainTextEdit* editor, QObject* parent = nullptr);

    QTextCursor textCursor() const override;
    QString documentText() const override;
    quint64 documentRevision() const override;

    void insertText(const QString& text) override;

    bool hasFocus() const override;
    bool isReadOnly() const override;

private:
    QPlainTextEdit* m_editor = nullptr; // не владеет
};
