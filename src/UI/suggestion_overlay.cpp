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
    const QStringList lines = m_suggestion.split(QLatin1Char('\n'));

    for (int i = 0; i < lines.size(); ++i) {
        // Первая строка — сразу за курсором; последующие — по левому
        // краю текстовой области (как в Copilot-подобных редакторах).
        const int x = (i == 0) ? caret.left() : margin;
        const qreal w = qreal(width()) - x;
        if (w <= 0.0)
            continue; // курсор уехал за правый край — рисовать некуда

        const qreal y = caret.top() + i * qreal(fm.lineSpacing());
        painter.drawText(QRectF(QPointF(x, y), QSizeF(w, fm.height())),
                         Qt::AlignLeft | Qt::AlignTop, lines[i]);
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
