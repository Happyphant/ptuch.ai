// suggestion_overlay.cpp
#include "suggestion_overlay.h"

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextDocument>

namespace {
// Альфа ghost-текста: заметно полупрозрачный, но читаемый.
constexpr int kGhostAlpha = 112; // ~44%

// Длина префикса text, помещающегося в maxWidth: ширина префикса
// монотонна по длине — бинарный поиск по индексам символов.
int fittedPrefixLength(const QFontMetrics& fm, const QString& text,
                       int maxWidth)
{
    int lo = 0;
    int hi = text.size();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (fm.horizontalAdvance(text.left(mid)) <= maxWidth)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}
} // namespace

SuggestionOverlay::SuggestionOverlay(QPlainTextEdit* editor)
    : QWidget(editor->viewport())
    , m_editor(editor)
{
    Q_ASSERT(m_editor != nullptr);

    // Фон невидим — рисуем только текст поверх содержимого редактора.
    setAttribute(Qt::WA_TranslucentBackground);
    // Мыши и фокусу не мешаем: клики/клавиши идут в редактор.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);

    setGeometry(m_editor->viewport()->rect());
    hide(); // до setSuggestion() пусто

    m_editor->viewport()->installEventFilter(this);

    // Перерисовка при всём, что двигает курсор, текст или прокрутку.
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged,
            this, [this]() { update(); });
    connect(m_editor, &QPlainTextEdit::textChanged,
            this, [this]() { update(); });
    connect(m_editor, &QPlainTextEdit::updateRequest,
            this, [this]() { update(); });
    connect(m_editor->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { update(); });
    connect(m_editor->horizontalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { update(); });
}

void SuggestionOverlay::setSuggestion(const QString& text)
{
    m_suggestion = text;

    if (m_suggestion.isEmpty()) {
        hide();
        return;
    }

    raise(); // поверх содержимого viewport'а
    show();
    update();
}

void SuggestionOverlay::clear()
{
    setSuggestion(QString());
}

void SuggestionOverlay::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    if (m_suggestion.isEmpty() || m_editor == nullptr)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QFont font = m_editor->font();
    painter.setFont(font);
    const QFontMetrics fm(font);

    // Полупрозрачный оттенок цвета текста редактора.
    QColor ghost = m_editor->palette().color(QPalette::Text);
    ghost.setAlpha(kGhostAlpha);
    painter.setPen(ghost);

    // Курсор в координатах viewport (overlay заполняет viewport целиком).
    const QRect caret = m_editor->cursorRect();
    const int margin = qRound(m_editor->document()->documentMargin());
    // Правая граница текстовой области: подсказка не должна уезжать
    // за правый край экрана — всё, что не влезает, переносится вниз.
    const int right = width() - margin;
    const qreal lineStep = fm.lineSpacing();

    const auto drawRow = [&](const QString& text, qreal rowX,
                             qreal rowY) {
        painter.drawText(QRectF(QPointF(rowX, rowY),
                                QSizeF(right - rowX, fm.height())),
                         Qt::AlignLeft | Qt::AlignTop, text);
    };

    // Первая строка — сразу за курсором; каждая следующая (в т.ч.
    // продолжение перенесённой) — по левому краю текстовой области
    // (как в Copilot-подобных редакторах). Длинная подсказка ломается
    // на строки по пробелам (слово длиннее строки — жёстко), поэтому
    // текст всегда остаётся во viewport, а не обрезается у края.
    qreal y = caret.top();
    int x = caret.left();

    const QStringList paragraphs =
        m_suggestion.split(QLatin1Char('\n'));
    for (const QString& paragraph : paragraphs) {
        QString remaining = paragraph;
        while (true) {
            const int available = right - x;
            if (available <= 0) {
                // Места в строке нет (курсор у правого края) —
                // продолжение уводим ниже, к левому краю.
                x = margin;
                y += lineStep;
                if (right - x <= 0)
                    return; // во viewport нет места совсем
                continue;
            }

            if (fm.horizontalAdvance(remaining) <= available) {
                drawRow(remaining, x, y);
                break;
            }

            // Не влезает целиком: отрезаем кусок до места разрыва —
            // по последнему пробелу, иначе жёстко по символу.
            int cut = fittedPrefixLength(fm, remaining, available);
            const int space =
                remaining.lastIndexOf(QLatin1Char(' '), cut);
            if (space > 0)
                cut = space;
            if (cut <= 0)
                cut = 1; // гарантия прогресса: символ шире остатка

            drawRow(remaining.left(cut), x, y);
            remaining = remaining.mid(cut);
            while (remaining.startsWith(QLatin1Char(' ')))
                remaining.remove(0, 1); // пробел разрыва не рисуем

            x = margin;
            y += lineStep;
        }
        // Следующий абзац (после '\n') начинается с новой строки.
        x = margin;
        y += lineStep;
    }
}

bool SuggestionOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_editor->viewport() && event->type() == QEvent::Resize) {
        const auto* resize = static_cast<QResizeEvent*>(event);
        setGeometry(QRect(QPoint(0, 0), resize->size()));
        update();
    }
    return QWidget::eventFilter(watched, event);
}
