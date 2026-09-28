// ghost_suggestion_test.cpp
#include <QtTest>

#include "UI/MainWindow.h"
#include "UI/suggestion_overlay.h"
#include "llama/llama_backend.h"
#include "suggestion/suggestion_controller.h"

#include <QFontMetrics>
#include <QImage>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QStatusBar>
#include <QTextCursor>
#include <QTextDocument>

namespace {

// Результат сканирования прямоугольника рендера overlay:
// найденные пиксели ghost-текста (alpha > kGhostPixelThreshold).
struct GhostBounds {
    bool found = false;
    int left = 0;
    int top = 0;
    int maxAlpha = 0;
};

constexpr int kGhostPixelThreshold = 8;

// Рисует overlay в прозрачный pixmap: пиксели с alpha > 0 — ghost-текст.
QPixmap renderOverlay(SuggestionOverlay& overlay)
{
    QPixmap pm(overlay.size());
    pm.fill(Qt::transparent);
    overlay.render(&pm);
    return pm;
}

GhostBounds scanRegion(const QPixmap& pm, const QRect& region)
{
    const QImage img = pm.toImage();
    GhostBounds bounds;

    const int x0 = qMax(0, region.left());
    const int y0 = qMax(0, region.top());
    const int x1 = qMin(img.width() - 1, region.right());
    const int y1 = qMin(img.height() - 1, region.bottom());

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const int alpha = img.pixelColor(x, y).alpha();
            if (alpha > kGhostPixelThreshold) {
                if (!bounds.found) {
                    bounds.found = true;
                    bounds.left = x;
                    bounds.top = y;
                }
                bounds.left = qMin(bounds.left, x);
                bounds.top = qMin(bounds.top, y);
                bounds.maxAlpha = qMax(bounds.maxAlpha, alpha);
            }
        }
    }
    return bounds;
}

// Редактор с текстом, курсором в конце и показанным viewport'ом.
void prepareEditor(QPlainTextEdit& editor)
{
    editor.resize(400, 200);
    editor.setPlainText(QStringLiteral("hello"));
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.show();
}
} // namespace

// ---------------------------------------------------------------------------
// Проверки ghost-подсказки:
//  1) overlay рисует полупрозрачный текст у курсора, НЕ меняя документ;
//  2) overlay не перехватывает мышь/фокус;
//  3) многострочные подсказки рисуются корректно;
//  4) сценарий клавиш в MainWindow: Tab принимает, Escape отклоняет,
//     Tab без подсказки не перехватывается, ввод/движение курсора чистят
//     подсказку.
// ---------------------------------------------------------------------------
class GhostSuggestionTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void overlayRendersGhostWithoutTouchingDocument();
    void overlayIgnoresMouseAndFocus();
    void overlaySupportsMultilineSuggestion();
    void keysScenario();
    void mainWindowLoadsRealModelAndClosesCleanly();
};

void GhostSuggestionTest::initTestCase()
{
    // UI-тесты работают на mock-бэкенде: реальный GGUF (2 ГБ) в
    // MainWindow не загружаем — иначе каждый тест тянул бы модель.
    qputenv("PTUCH_DISABLE_LLAMA", "1");
}

void GhostSuggestionTest::overlayRendersGhostWithoutTouchingDocument()
{
    QPlainTextEdit editor;
    prepareEditor(editor);
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    SuggestionOverlay overlay(&editor);
    overlay.setSuggestion(QStringLiteral("ghost"));

    // Документ не тронут; overlay показан и покрывает весь viewport.
    QCOMPARE(editor.toPlainText(), QStringLiteral("hello"));
    QVERIFY(overlay.isVisible());
    QCOMPARE(overlay.geometry(), editor.viewport()->rect());

    const QPixmap rendered = renderOverlay(overlay);
    const QRect caret = editor.cursorRect();
    const int margin = qRound(editor.document()->documentMargin());
    const int lineStep = QFontMetrics(editor.font()).lineSpacing();

    // Ghost нарисован около позиции курсора (попуск на side bearing глифа).
    const GhostBounds firstLine = scanRegion(
        rendered, QRect(QPoint(caret.left() - 1, caret.top() - 1),
                        QSize(200, lineStep)));
    QVERIFY2(firstLine.found, "Ghost-текст не нарисован у курсора");
    QVERIFY(firstLine.left <= caret.left() + 6);
    QVERIFY(firstLine.top <= caret.top() + 6);

    // Полупрозрачный: видимый (alpha > 30), но не непрозрачный —
    // pen задан с alpha 112, выше него пикселей быть не может.
    QVERIFY(firstLine.maxAlpha >= 30);
    QVERIFY(firstLine.maxAlpha <= 130);

    // Однострочная подсказка: второй строки нет. Зона проверки — левее
    // курсора (там, где рисовались бы строки после первой), поэтому
    // glyphs самой первой строки сюда не попадают.
    const GhostBounds noSecondLine = scanRegion(
        rendered, QRect(QPoint(margin - 2, caret.top() + lineStep),
                        QSize(qMax(1, caret.left() - margin), lineStep)));
    QVERIFY(!noSecondLine.found);

    // Очистка скрывает overlay, документ по-прежнему цел.
    overlay.clear();
    QVERIFY(!overlay.isVisible());
    QCOMPARE(editor.toPlainText(), QStringLiteral("hello"));
}

void GhostSuggestionTest::overlayIgnoresMouseAndFocus()
{
    QPlainTextEdit editor;
    editor.resize(200, 100);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    SuggestionOverlay overlay(&editor);

    // Клики и фокус проходят мимо overlay — ввод достаётся редактору.
    QVERIFY(overlay.testAttribute(Qt::WA_TransparentForMouseEvents));
    QCOMPARE(overlay.focusPolicy(), Qt::NoFocus);
    // До setSuggestion() подсказка не показывается.
    QVERIFY(!overlay.isVisible());
}

void GhostSuggestionTest::overlaySupportsMultilineSuggestion()
{
    QPlainTextEdit editor;
    prepareEditor(editor);
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    SuggestionOverlay overlay(&editor);
    overlay.setSuggestion(QStringLiteral("first\nsecond"));

    // Документ не изменён ghost-подсказкой.
    QCOMPARE(editor.toPlainText(), QStringLiteral("hello"));

    const QPixmap rendered = renderOverlay(overlay);
    const QRect caret = editor.cursorRect();
    const int margin = qRound(editor.document()->documentMargin());
    const int lineStep = QFontMetrics(editor.font()).lineSpacing();

    // Первая строка — у курсора.
    const GhostBounds line1 = scanRegion(
        rendered, QRect(QPoint(caret.left() - 1, caret.top() - 1),
                        QSize(200, lineStep)));
    QVERIFY2(line1.found, "Первая строка ghost не нарисована");

    // Вторая строка — по левому краю текстовой области, ниже первой
    // (зона строго левее курсора: glyphs первой строки туда не заглядывают).
    const int zoneWidth = qMax(1, caret.left() - margin);
    const GhostBounds line2 = scanRegion(
        rendered, QRect(QPoint(margin - 2, caret.top() + lineStep),
                        QSize(zoneWidth, lineStep)));
    QVERIFY2(line2.found, "Вторая строка ghost не нарисована");
    QVERIFY(line2.left <= margin + 4);

    // Третьей строки у двухстрочной подсказки быть не должно.
    const GhostBounds line3 = scanRegion(
        rendered, QRect(QPoint(margin - 2, caret.top() + 2 * lineStep),
                        QSize(zoneWidth, lineStep)));
    QVERIFY(!line3.found);

    QCOMPARE(editor.toPlainText(), QStringLiteral("hello"));
}

void GhostSuggestionTest::keysScenario()
{
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* editor = window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
    QVERIFY(editor);
    auto* controller = window.findChild<SuggestionController*>();
    QVERIFY(controller);
    auto* overlay = window.findChild<SuggestionOverlay*>();
    QVERIFY(overlay);

    // Фокус обязателен: без него контроллер не стартует debounce.
    window.activateWindow();
    editor->setFocus();
    QVERIFY(QTest::qWaitFor([&]() { return editor->hasFocus(); }, 3000));

    // --- Печать -> debounce (500 мс) -> mock в рабочем потоке (~300 мс).
    QTest::keyClicks(editor, QStringLiteral("hello"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("hello"));

    QVERIFY(QTest::qWaitFor([&]() { return controller->hasSuggestion(); }, 5000));
    QVERIFY(overlay->isVisible());
    // Ghost не в документе.
    QCOMPARE(editor->toPlainText(), QStringLiteral("hello"));

    // Опциональный снимок для ручной проверки: PTUCH_GHOST_SCREENSHOT=/path.png
    const QString screenshotPath = qEnvironmentVariable("PTUCH_GHOST_SCREENSHOT");
    if (!screenshotPath.isEmpty())
        QVERIFY(window.grab().save(screenshotPath));

    // --- Tab принимает подсказку (вставляется в документ).
    QTest::keyClick(editor, Qt::Key_Tab);
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(!overlay->isVisible());
    QVERIFY(editor->toPlainText().startsWith(QStringLiteral("hello continuation")));

    // --- Tab без подсказки НЕ перехватывается: вставляется табуляция.
    QTest::keyClick(editor, Qt::Key_Tab);
    QVERIFY2(editor->toPlainText().contains(QChar('\t')),
             "Tab без подсказки должен достаться редактору");

    // --- Ввод обычного символа чистит показанную подсказку.
    QVERIFY(QTest::qWaitFor([&]() { return controller->hasSuggestion(); }, 5000));
    QVERIFY(overlay->isVisible());
    QTest::keyClick(editor, Qt::Key_Z);
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(!overlay->isVisible());
    QVERIFY(editor->toPlainText().endsWith(QChar('z')));

    // --- Escape отклоняет подсказку: документ не меняется.
    QVERIFY(QTest::qWaitFor([&]() { return controller->hasSuggestion(); }, 5000));
    QVERIFY(overlay->isVisible());
    const QString beforeEscape = editor->toPlainText();
    QTest::keyClick(editor, Qt::Key_Escape);
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(!overlay->isVisible());
    QCOMPARE(editor->toPlainText(), beforeEscape);

    // Новый ввод запускает debounce заново — нужна свежая подсказка
    // для проверки движения курсора.
    QTest::keyClick(editor, Qt::Key_Y);
    QVERIFY(editor->toPlainText().endsWith(QChar('y')));

    // --- Движение курсора чистит показанную подсказку.
    QVERIFY(QTest::qWaitFor([&]() { return controller->hasSuggestion(); }, 5000));
    QVERIFY(overlay->isVisible());
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::Start);
    editor->setTextCursor(cursor);
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(!overlay->isVisible());
}

void GhostSuggestionTest::mainWindowLoadsRealModelAndClosesCleanly()
{
    // Полный жизненный цикл с реальной GGUF: MainWindow сам находит
    // модель, грузит её в llama-потоке, подменяет mock через setBackend
    // и корректно освобождает llama_model/llama_context при закрытии
    // (closeEvent -> stopLlamaWorker -> RAII в потоке объекта).
    // Медленный (2 ГБ): включается PTUCH_MODEL_TESTS=1.
    if (!qEnvironmentVariableIsSet("PTUCH_MODEL_TESTS"))
        QSKIP("Тест с реальной GGUF включается PTUCH_MODEL_TESTS=1");
    if (resolveModelPath().isEmpty())
        QSKIP("GGUF-модель не найдена (models/ или настройки)");

    // Здесь llama включён; initTestCase отключает его для остальных
    // сценариев (восстанавливается после блока).
    qunsetenv("PTUCH_DISABLE_LLAMA");

    {
        MainWindow window;
        window.resize(800, 600);
        window.show();

        // Загрузка асинхронна: ждём сигнала modelLoaded в статусной
        // строке (UI-поток не блокируется — ожидание через event loop).
        QVERIFY2(QTest::qWaitFor(
                     [&]() {
                         return window.statusBar()->currentMessage().contains(
                             QStringLiteral("Модель загружена"));
                     },
                     120000),
                 "Модель не загрузилась за 120 с");

        // Закрытие окна проходит closeEvent: controller shutdown ->
        // stopLlamaWorker (requestStop + quit/wait) -> освобождение
        // модели/контекста. Падение здесь завалит тест.
        window.close();
        QVERIFY(!window.isVisible());
    } // деструктор MainWindow: повторный stopLlamaWorker — no-op

    qputenv("PTUCH_DISABLE_LLAMA", "1");
}

QTEST_MAIN(GhostSuggestionTest)

#include "ghost_suggestion_test.moc"
