// suggestion_overlay.h
#pragma once

#include <QString>
#include <QWidget>

class QPlainTextEdit;

// Ghost-подсказка: полупрозрачный текст поверх viewport'а редактора,
// нарисованный в позиции курсора. Overlay НИКОГДА не меняет документ —
// он только рисует; содержимое приходит извне (MainWindow связывает
// сигналы suggestionReady / suggestionCleared контроллера).
//
// Координаты: overlay — ребёнок editor->viewport() с геометрией во весь
// viewport, поэтому paintEvent работает в координатах viewport'а.
// Измерение (см. также test): QPlainTextEdit::cursorRect() возвращает
// прямоугольник именно в координатах viewport'а — даже когда stylesheet
// сдвигает viewport внутрь виджета (padding: 8px), cursorRect не меняется.
//
// Перенос: подсказка никогда не уезжает за правый край экрана — текст
// ломается на строки (первая — от курсора до правого края, продолжение
// — по левому краю; разрыв по пробелам, слово длиннее строки — жёсткий).
//
// Мышь/фокус: WA_TransparentForMouseEvents + NoFocus — ввод идёт в
// редактор как обычно; Tab/Escape обрабатывает MainWindow (eventFilter)
// и только когда подсказка реально показана.
//
// Жизненный цикл: создаётся MainWindow один раз, живёт как ребёнок
// viewport'а (разрушается вместе с редактором).
class SuggestionOverlay final : public QWidget
{
    Q_OBJECT

public:
    // editor не владеет; сам overlay становится ребёнком его viewport'а.
    explicit SuggestionOverlay(QPlainTextEdit* editor);

    // Показать подсказку. Пустой текст скрывает overlay.
    void setSuggestion(const QString& text);
    // То же, что setSuggestion(QString()) — удобно для сигнала cleared.
    void clear();

    const QString& suggestion() const { return m_suggestion; }

protected:
    void paintEvent(QPaintEvent* event) override;
    // Следим за resize'ом viewport'а, чтобы overlay покрывал его целиком.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QPlainTextEdit* m_editor = nullptr; // не владеет
    QString m_suggestion;
};
