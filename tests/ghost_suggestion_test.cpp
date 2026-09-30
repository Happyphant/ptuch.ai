// ghost_suggestion_test.cpp
#include <QtTest>

#include "UI/MainWindow.h"
#include "UI/settings_dialog.h"
#include "UI/style_panel.h"
#include "UI/suggestion_overlay.h"
#include "UI/theme.h"
#include "llama/llama_backend.h"
#include "settings/app_settings.h"
#include "suggestion/suggestion_controller.h"

#include "qsettings_backup.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QFontMetrics>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

#if PTUCH_DIAGNOSTICS
// Режим диагностики: диалог снимка (слот ниже QSKIP при опции OFF).
#include "UI/diagnostics_dialog.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QLabel>
#endif

namespace {

// Только ключи нашего приложения: QSettings::allKeys() включает и
// fallback (NSGlobalDomain) — чужие ключи нельзя затирать или
// копировать в файл настроек PtuchEditor.
bool isOurSettingsKey(const QString& key)
{
    return key.startsWith(QLatin1String("llama/")) ||
           key.startsWith(QLatin1String("suggestion/")) ||
           key.startsWith(QLatin1String("ui/"));
}

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

// Пиксели в зоне ghost: фон (цвет Base viewport'а) и глифы подсказки
// (всё прочее с ненулевой alpha). Разделение нужно, потому что после
// появления фона ghost-глифы непрозрачны (нарисованы поверх Base) —
// по alpha их от фона уже не отличить.
struct GhostPixels {
    int background = 0; // пикселей цвета Base (фон под ghost-строкой)
    int glyphs = 0;     // пикселей, отличных от Base (глифы ghost)
};

GhostPixels classifyGhost(const QPixmap& pm, const QRect& region,
                          const QColor& base)
{
    // QColor::operator== сравнивает и спецификацию (Rgb/Hsl): палитра
    // может отдать цвет в другом spec, чем pixelColor. Сравниваем по
    // компонентам — это то, что реально нарисовано.
    const auto sameValue = [](const QColor& a, const QColor& b) {
        return a.alpha() == b.alpha() && a.red() == b.red()
               && a.green() == b.green() && a.blue() == b.blue();
    };

    const QImage img = pm.toImage();
    GhostPixels out;

    const int x0 = qMax(0, region.left());
    const int y0 = qMax(0, region.top());
    const int x1 = qMin(img.width() - 1, region.right());
    const int y1 = qMin(img.height() - 1, region.bottom());

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const QColor color = img.pixelColor(x, y);
            if (color.alpha() <= kGhostPixelThreshold)
                continue;
            if (sameValue(color, base))
                ++out.background;
            else
                ++out.glyphs;
        }
    }
    return out;
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

// Активация окна и ожидание фокуса в редакторе. macOS иногда
// игнорирует первую activateWindow (гонка за передний план, тесты
// запускаются из терминала) — повторяем попытку активации вплоть до
// таймаута, но фокус всё равно обязателен (без него контроллер не
// стартует debounce и клавиши пи мимо редактора).
bool activateAndFocus(MainWindow& window, QPlainTextEdit& editor)
{
    window.activateWindow();
    editor.setFocus();
    return QTest::qWaitFor(
        [&]() {
            if (!editor.hasFocus())
                window.activateWindow();
            return editor.hasFocus();
        },
        3000);
}
} // namespace

// ---------------------------------------------------------------------------
// Проверки ghost-подсказки:
//  1) overlay рисует ghost у курсора на непрозрачном фоне Base, НЕ
//     меняя документ, и остаётся выровненным после скролла viewport'а;
//  2) overlay не перехватывает мышь/фокус;
//  3) многострочные подсказки рисуются корректно;
//  4) длинная подсказка переносится во viewport, а не уезжает
//     за правый край;
//  5) сценарий клавиш в MainWindow: Tab принимает, Escape отклоняет,
//     Tab без подсказки не перехватывается, ввод/движение курсора чистят
//     подсказку.
// ---------------------------------------------------------------------------
class GhostSuggestionTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void overlayRendersGhostWithoutTouchingDocument();
    void overlayIgnoresMouseAndFocus();
    void overlaySupportsMultilineSuggestion();
    void overlayWrapsLongSuggestionToViewport();
    void overlayRealignsAfterViewportScroll();
    void keysScenario();
    void ctrlSpaceAndFocusOutScenario();
    void stateIndicatorShowsThemeColors();
    void systemSchemeRestoresPlatformDefaults();
    void documentOpenShowsNameAndClearsSuggestion();
    void documentSaveWritesUtf8AndClearsModified();
    void documentNewClearsEditorAndSuggestion();
    void closeEventPromptsForUnsavedChanges();
    void settingsDialogAppliesAndPersists();
    void styleMixChangeClearsGhostSuggestion();
    void diagnosticsButtonOpensDialog(); // gated: PTUCH_DIAGNOSTICS
    void mainWindowLoadsRealModelAndClosesCleanly();

private:
    // Пользовательские настройки: полный снимок файла QSettings
    // (снимок в initTestCase, дословный возврат в cleanupTestCase).
    QVariantMap m_original;
};

void GhostSuggestionTest::initTestCase()
{
    // UI-тесты работают на mock-бэкенде: реальный GGUF (2 ГБ) в
    // MainWindow не загружаем — иначе каждый тест тянул бы модель.
    qputenv("PTUCH_DISABLE_LLAMA", "1");

    // Самовосстановление после АВАРИЙНОГО прогона (QVERIFY/QTEST_ASSERT
    // фатальны: SIGABRT доходит до cleanupTestCase, и памятный снимок
    // теряется) — если файл-снимок остался, вернём пользовательские
    // ключи из него ДО нового снимка (см. qsettings_backup.h).
    QSettingsBackup::restoreIfCrashed(isOurSettingsKey);

    // Снимок ВСЕХ пользовательских настроек: тесты пишут в тот же
    // файл QSettings (PtuchAI/PtuchEditor) — в т.ч. через
    // applySettings (ключи llama/*), cleanupTestCase вернёт файл
    // дословно.
    //
    // Только ключи НАШЕГО приложения: allKeys() включает и fallback
    // (NSGlobalDomain) — их нельзя ни затирать, ни копировать в наш
    // файл настроек.
    QSettings settings(QStringLiteral("PtuchAI"),
                       QStringLiteral("PtuchEditor"));
    const QStringList keys = settings.allKeys();
    for (const QString& key : keys) {
        if (!isOurSettingsKey(key))
            continue;
        m_original.insert(key, settings.value(key));
        settings.remove(key);
    }

    // Дубль снимка в файл — страховка от аварийного завершения (см. выше).
    QSettingsBackup::save(m_original);

    // Настройки подсказок принудительно дефолтные: тайминги тестов
    // (500 мс debounce, авто-подсказки включены) не должны зависеть
    // от пользовательского QSettings.
    settings.setValue(AppSettings::keyDebounceMs, 500);
    settings.setValue(AppSettings::keyAutoSuggestions, true);
    settings.sync();
}

void GhostSuggestionTest::cleanupTestCase()
{
    // Возврат дословно только НАШИХ ключей (см. initTestCase).
    QSettings settings(QStringLiteral("PtuchAI"),
                       QStringLiteral("PtuchEditor"));
    const QStringList keys = settings.allKeys();
    for (const QString& key : keys) {
        if (isOurSettingsKey(key))
            settings.remove(key);
    }
    for (auto it = m_original.cbegin(); it != m_original.cend(); ++it)
        settings.setValue(it.key(), it.value());
    settings.sync();
    m_original.clear();
    // Возврат выполнен штатно — файл-снимок больше не нужен.
    QSettingsBackup::clear();
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
    const QRect firstRegion(QPoint(caret.left() - 1, caret.top() - 1),
                            QSize(200, lineStep));
    const GhostBounds firstLine = scanRegion(rendered, firstRegion);
    QVERIFY2(firstLine.found, "Ghost-текст не нарисован у курсора");
    QVERIFY(firstLine.left <= caret.left() + 6);
    QVERIFY(firstLine.top <= caret.top() + 6);

    // Под ghost-строкой непрозрачный фон цвета Base viewport'а: документ
    // под подсказкой не просвечивает и не наезжает на призрак (вторая
    // половина строки после курсора читается раздельно с ghost). Глифы
    // подсказки при этом нарисованы — пиксели, отличные от Base.
    QCOMPARE(firstLine.maxAlpha, 255);
    const QColor base = editor.viewport()->palette().color(QPalette::Base);
    const GhostPixels painted = classifyGhost(rendered, firstRegion, base);
    QVERIFY2(painted.background > 0,
             "Под ghost-строкой нет фона цвета Base viewport'а");
    QVERIFY2(painted.glyphs > 0, "Ghost-глифы не нарисованы поверх фона");

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

void GhostSuggestionTest::overlayWrapsLongSuggestionToViewport()
{
    QPlainTextEdit editor;
    prepareEditor(editor); // 400×200, курсор в конце строки
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    SuggestionOverlay overlay(&editor);
    // Длинная подсказка без '\n': раньше она рисовалась одной строкой
    // от курсора и уезжала за правый край экрана (обрезалась
    // viewport'ом) — теперь должна перенестись вниз.
    overlay.setSuggestion(QStringLiteral(
        " very long suggestion text that certainly does not fit into "
        "the narrow editor viewport and must be wrapped into several "
        "visual lines without running off the right screen edge"));
    QVERIFY(overlay.isVisible());

    // Документ не тронут ghost-подсказкой.
    QCOMPARE(editor.toPlainText(), QStringLiteral("hello"));

    const QPixmap rendered = renderOverlay(overlay);
    const QRect caret = editor.cursorRect();
    const int margin = qRound(editor.document()->documentMargin());
    const int lineStep = QFontMetrics(editor.font()).lineSpacing();

    // Продолжение перенесённой строки — у левого края текстовой
    // области, ниже строки курсора (зона строго левее курсора:
    // glyphs первой строки туда не заглядывают). У старого кода
    // (одна строка от курсора) этой строки не было вовсе.
    const int zoneWidth = qMax(1, caret.left() - margin);
    const GhostBounds continuation = scanRegion(
        rendered, QRect(QPoint(margin - 2, caret.top() + lineStep),
                        QSize(zoneWidth, lineStep)));
    QVERIFY2(continuation.found,
             "Длинная подсказка не перенесена во вторую строку");
    QVERIFY(continuation.left <= margin + 4);

    // Первая строка рисуется от курсора, а не прижата к левому краю.
    const GhostBounds firstLine = scanRegion(
        rendered, QRect(QPoint(caret.left() - 1, caret.top() - 1),
                        QSize(200, lineStep)));
    QVERIFY2(firstLine.found, "Первая строка ghost не нарисована");

    QCOMPARE(editor.toPlainText(), QStringLiteral("hello"));
}

// Скролл viewport'а: QPlainTextEdit прокручивает через QWidget::scroll(),
// а тот сдвигает дочерних (QWidgetPrivate::scrollChildren) — исторически
// overlay уезжал из-под курсора и ghost рисовался мимо строки. После
// moveEvent-фикса геометрия восстанавливается синхронно, а ghost — в
// НОВОЙ позиции курсора (cursorRect пересчитывается при скролле).
void GhostSuggestionTest::overlayRealignsAfterViewportScroll()
{
    QPlainTextEdit editor;
    editor.resize(300, 120);

    // Документ сильно выше viewport — прокрутка возможна.
    QStringList lines;
    for (int i = 0; i < 100; ++i)
        lines.append(QStringLiteral("line %1 text for scrolling").arg(i));
    editor.setPlainText(lines.join(QLatin1Char('\n')));
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    // Курсор посреди документа — ПОСЛЕ show(): до layout у scroll-баров
    // нет диапазонов, и ensureCursorVisible ничего не прокручивает.
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, 50);
    editor.setTextCursor(cursor);
    editor.ensureCursorVisible();

    SuggestionOverlay overlay(&editor);
    overlay.setSuggestion(QStringLiteral("ghost"));
    QVERIFY(overlay.isVisible());
    QCOMPARE(overlay.geometry(), editor.viewport()->rect());

    // Скролл вниз по документу: valueChanged -> scrollContentsBy ->
    // setTopBlock -> viewport->scroll(dx, dy) -> scrollChildren со
    // сдвигом детей и QMoveEvent.
    auto* bar = editor.verticalScrollBar();
    QVERIFY(bar->maximum() > bar->value() + 3);
    bar->setValue(bar->value() + 3);

    // Без восстановления геометрии overlay остался бы со сдвигом dy
    // ( дети viewport'а сдвинуты scrollChildren ).
    QCOMPARE(overlay.geometry(), editor.viewport()->rect());

    // Ghost нарисован у курсора в ЕГО новой позиции viewport'а.
    const QRect caret = editor.cursorRect();
    QVERIFY2(caret.bottom() <= editor.viewport()->height(),
             "Тест: курсор должен остаться видим после скролла");

    const QPixmap rendered = renderOverlay(overlay);
    const int lineStep = QFontMetrics(editor.font()).lineSpacing();
    const GhostBounds bounds = scanRegion(
        rendered, QRect(QPoint(caret.left() - 1, caret.top() - 1),
                        QSize(200, lineStep)));
    QVERIFY2(bounds.found, "Ghost не следует за курсором после скролла");
    QVERIFY(bounds.left <= caret.left() + 6);
    QVERIFY(bounds.top <= caret.top() + 6);

    // Документ цел: overlay только рисует.
    QCOMPARE(editor.toPlainText(), lines.join(QLatin1Char('\n')));
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
    QVERIFY(activateAndFocus(window, *editor));

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

// Ctrl+Space и потеря фокуса в живом MainWindow:
//  1) Ctrl+Space стартует генерацию немедленно — доказательство: при
//     искусственно увеличенном debounce (10 с) подсказки по таймеру не
//     приходит, а после Ctrl+Space состояние меняется синхронно;
//  2) Tab принимает ТОЛЬКО актуальную подсказку: новый символ её гасит —
//     в документ идёт табуляция, а не устаревший текст;
//  3) потеря фокуса прячет показанную подсказку (overlay + Idle);
//  4) закрытие окна при активной генерации: closeEvent -> shutdown
//     (cancel + отвязка backend) + остановка потоков — без крашей.
void GhostSuggestionTest::ctrlSpaceAndFocusOutScenario()
{
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* editor =
        window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
    QVERIFY(editor);
    auto* controller = window.findChild<SuggestionController*>();
    QVERIFY(controller);
    auto* overlay = window.findChild<SuggestionOverlay*>();
    QVERIFY(overlay);

    QVERIFY(activateAndFocus(window, *editor));

    // Намеренно огромный debounce: подсказка не может прийти по таймеру.
    controller->setDebounceInterval(10000);

    // Печать уводит в Debouncing, но запрос не уходит (10 с не ждём).
    QTest::keyClicks(editor, QStringLiteral("hello"));
    QCOMPARE(controller->state(),
             SuggestionController::State::Debouncing);
    QTest::qWait(600);
    QVERIFY2(!controller->hasSuggestion(),
             "При debounce 10 с подсказка не должна была прийти");

    // Ctrl+Space — немедленный запрос: состояние меняется синхронно,
    // debounce (10 с) минуется.
    QTest::keyClick(editor, Qt::Key_Space, Qt::ControlModifier);
    QCOMPARE(controller->state(),
             SuggestionController::State::Generating);
    QVERIFY(QTest::qWaitFor(
        [&]() { return controller->hasSuggestion(); }, 5000));
    QVERIFY(overlay->isVisible());
    const QString shown = controller->suggestion();
    QVERIFY(!shown.isEmpty());

    // Новый символ гасит подсказку -> Tab принимать нечего: в документ
    // идёт табуляция, а не устаревший текст подсказки.
    QTest::keyClick(editor, Qt::Key_X);
    QVERIFY(!controller->hasSuggestion());
    QTest::keyClick(editor, Qt::Key_Tab);
    QVERIFY2(editor->toPlainText().contains(QChar('\t')),
             "Tab без актуальной подсказки должен достаться редактору");
    QVERIFY2(!editor->toPlainText().contains(shown),
             "Устаревшая подсказка не должна вставляться Tab'ом");

    // Свежая подсказка для проверки потери фокуса — снова Ctrl+Space.
    QTest::keyClick(editor, Qt::Key_Space, Qt::ControlModifier);
    QCOMPARE(controller->state(),
             SuggestionController::State::Generating);
    QVERIFY(QTest::qWaitFor(
        [&]() { return controller->hasSuggestion(); }, 5000));
    QVERIFY(overlay->isVisible());

    // Смена фокуса (переход на кнопку панели): подсказка скрыта,
    // контроллер в Idle — её нельзя принять Tab'ом из чужого виджета.
    auto* generateButton = window.findChild<QPushButton*>(
        QStringLiteral("generateButton"));
    QVERIFY(generateButton);
    generateButton->setFocus();
    QVERIFY(QTest::qWaitFor([&]() { return !editor->hasFocus(); }, 3000));
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(!overlay->isVisible());
    QCOMPARE(controller->state(),
             SuggestionController::State::Idle);

    // Закрытие окна при активной генерации: requestSuggestion стартует
    // и без фокуса (ручной путь), closeEvent гасит shutdown'ом.
    QTest::keyClick(editor, Qt::Key_Space, Qt::ControlModifier);
    QCOMPARE(controller->state(),
             SuggestionController::State::Generating);

    // Без несохранённых изменений — иначе closeEvent спросит про
    // сохранение (подтверждение при закрытии проверяется отдельно в
    // closeEventPromptsForUnsavedChanges).
    editor->document()->setModified(false);
    window.close();
    QCOMPARE(controller->state(),
             SuggestionController::State::Idle);
    QVERIFY(!controller->hasSuggestion());
    // Деструктор на выходе из скоупа: shutdown (идемпотентно) +
    // quit/wait потоков — при нарушении тест зависнет или упадёт.
}

// Индикатор состояния в живом MainWindow использует цвета темы
// (схема ПТЮЧ по умолчанию): стартовое Ready (зелёный), печать ->
// Waiting (янтарный), пустой документ + Ctrl+Space -> Error (красный).
// Состояние Generating (электрик-синий) закрыто в tests/theme_test.cpp
// (Theme::stateColor): окно генерации в UI слишком мало для
// детерминированной ловли.
void GhostSuggestionTest::stateIndicatorShowsThemeColors()
{
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* editor =
        window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
    QVERIFY(editor);
    auto* controller = window.findChild<SuggestionController*>();
    QVERIFY(controller);
    auto* indicator = window.findChild<QLabel*>(
        QStringLiteral("stateIndicator"));
    QVERIFY(indicator);

    using State = SuggestionController::State;

    // Стартовое состояние: Ready с зелёным цветом схемы ПТЮЧ
    // (initTestCase чистит ui/style -> дефолт «ptuch»).
    QCOMPARE(indicator->text(), QStringLiteral("Ready"));
    QVERIFY(indicator->styleSheet().contains(
        Theme::stateColor(Theme::Scheme::Ptuch, State::Idle)
            .name(QColor::HexRgb)));

    QVERIFY(activateAndFocus(window, *editor));
    // Намеренно огромный debounce: состояние Debouncing удерживается
    // (по таймеру подсказка не приходит).
    controller->setDebounceInterval(10000);

    // Печать -> Debouncing: Waiting + янтарный цвет схемы.
    QTest::keyClicks(editor, QStringLiteral("hello"));
    QCOMPARE(controller->state(), State::Debouncing);
    QCOMPARE(indicator->text(), QStringLiteral("Waiting"));
    QVERIFY(indicator->styleSheet().contains(
        Theme::stateColor(Theme::Scheme::Ptuch, State::Debouncing)
            .name(QColor::HexRgb)));

    // Пустой документ + Ctrl+Space -> «Пустой контекст»: ручной запуск
    // не может пройти мимо — состояние уходит в Error (красный схемы).
    editor->clear();
    QTest::keyClick(editor, Qt::Key_Space, Qt::ControlModifier);
    QCOMPARE(controller->state(), State::Error);
    QCOMPARE(indicator->text(), QStringLiteral("Error"));
    QVERIFY(indicator->styleSheet().contains(
        Theme::stateColor(Theme::Scheme::Ptuch, State::Error)
            .name(QColor::HexRgb)));
}

// Системная схема («default») через комбобокс стиля в Settings:
// применяется сразу — stylesheet'ы окна и панели сняты, индикатор
// без инлайнового цвета, палитра приложения возвращается к системному
// снимку до темизации (цвета шрифтов и контролов платформенные).
void GhostSuggestionTest::systemSchemeRestoresPlatformDefaults()
{
    // Снимок ДО создания окна: конструктор MainWindow снимает палитру
    // первым действием, а System возвращает её же — до и после равны.
    const QPalette before = qApp->palette();
    // Возврат схемы — даже при падении QVERIFY (иначе каскад сбоев
    // в последующих тестах).
    const QString previousStyle = AppSettings::load().style;
    const auto restoreStyle = qScopeGuard([&]() {
        AppSettings values = AppSettings::load();
        values.style = previousStyle;
        values.save();
    });

    MainWindow window;
    auto* settingsButton = window.findChild<QPushButton*>(
        QStringLiteral("settingsButton"));
    QVERIFY2(settingsButton, "Кнопка Settings не создана");

    bool styleApplied = false;
    // click() -> openSettings() -> dialog.exec() (вложенный event
    // loop): таймер срабатывает внутри exec и закрывает диалог.
    QTimer::singleShot(0, &window, [&]() {
        auto* dialog = window.findChild<SettingsDialog*>(
            QStringLiteral("settingsDialog"));
        if (dialog == nullptr)
            return;
        auto* styleCombo = dialog->findChild<QComboBox*>(
            QStringLiteral("styleCombo"));
        if (styleCombo == nullptr)
            return;
        const int index = styleCombo->findData(
            QLatin1String(AppSettings::styleSystem));
        if (index < 0)
            return;
        styleCombo->setCurrentIndex(index);
        styleApplied = true;
        dialog->accept();
    });

    settingsButton->click(); // модальный exec до accept()
    QVERIFY2(styleApplied, "Комбобокс стиля не найден");

    // 1) Схема сохранена (та же строка, что читает Theme::schemeFromId).
    QCOMPARE(AppSettings::load().style,
             QLatin1String(AppSettings::styleSystem));

    // 2) Палитра приложения — снимок до темизации (цвета шрифтов
    //    вернулись к системным).
    QCOMPARE(qApp->palette(), before);

    // 3) Stylesheet'ы сняты: окно и панель стилизованы системно.
    QVERIFY(window.styleSheet().isEmpty());
    auto* panel = window.findChild<StylePanel*>(
        QStringLiteral("stylePanel"));
    QVERIFY(panel);
    QVERIFY(panel->styleSheet().isEmpty());

    // Маркеры профилей: цвет профиля (данные) остаётся, декора нет.
    auto* marker = window.findChild<QLabel*>(
        QStringLiteral("colorMarker_pushkin"));
    QVERIFY(marker);
    QVERIFY(marker->styleSheet().contains(
        QStringLiteral("background-color")));
    QVERIFY(!marker->styleSheet().contains(
        QStringLiteral("border-radius")));

    // 4) Индикатор состояния: заголовок есть, инлайнового цвета нет.
    auto* indicator = window.findChild<QLabel*>(
        QStringLiteral("stateIndicator"));
    QVERIFY(indicator);
    QCOMPARE(indicator->text(), QStringLiteral("Ready"));
    QVERIFY(indicator->styleSheet().isEmpty());
}

// Открытие файла (ядро openFile, без диалога):
//  - русский текст файла в редакторе (UTF-8), modified сброшен;
//  - имя файла и «не изменён» видны в индикаторе и заголовке;
//  - показанная подсказка очищена (overlay скрыт, Idle);
//  - генерация НЕ стартует ни во время загрузки, ни после неё:
//    debounce не запланирован — состояние остаётся Idle дольше
//    интервала (500 мс из initTestCase).
void GhostSuggestionTest::documentOpenShowsNameAndClearsSuggestion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("notes.txt"));
    const QString content = QStringLiteral("Русский текст открытия: ёжик.");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content.toUtf8()), content.toUtf8().size());
    }

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* editor =
        window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
    auto* controller = window.findChild<SuggestionController*>();
    auto* overlay = window.findChild<SuggestionOverlay*>();
    auto* indicator =
        window.findChild<QLabel*>(QStringLiteral("documentIndicator"));
    QVERIFY(editor && controller && overlay && indicator);

    QVERIFY(activateAndFocus(window, *editor));

    // Показываем подсказку и правим текст (документ «изменён»).
    QTest::keyClicks(editor, QStringLiteral("hello"));
    QVERIFY(QTest::qWaitFor([&]() { return controller->hasSuggestion(); },
                            5000));
    QVERIFY(overlay->isVisible());
    QVERIFY(editor->document()->isModified());
    // maybeSave не должен мешать: подтверждение несохранённых
    // изменений проверяется в closeEventPromptsForUnsavedChanges.
    editor->document()->setModified(false);

    // Открываем ПОКА подсказка показана и debounce активен.
    QVERIFY(window.openFile(path));

    // Документ: текст файла, modified сброшен, имя в индикаторе/заголовке.
    QCOMPARE(editor->toPlainText(), content);
    QVERIFY(!editor->document()->isModified());
    QVERIFY(!window.isWindowModified());
    QVERIFY(indicator->text().contains(QStringLiteral("notes.txt")));
    QVERIFY(indicator->text().contains(QStringLiteral(" • не изменён")));
    QVERIFY(window.windowTitle().contains(QStringLiteral("notes.txt")));

    // Подсказка очищена, контроллер снова включён и в Idle.
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(!overlay->isVisible());
    QVERIFY(controller->isEnabled());
    QCOMPARE(controller->state(), SuggestionController::State::Idle);

    // Генерация во время/после загрузки не стартует: ждём дольше
    // debounce — ни состояния Debouncing/Generating, ни подсказки.
    QTest::qWait(900);
    QCOMPARE(controller->state(), SuggestionController::State::Idle);
    QVERIFY(!controller->hasSuggestion());
}

// Ctrl+S (actionSave) при открытом файле: диалог не нужен, русский
// текст пишется байтами UTF-8, modified гаснет, индикатор показывает
// «не изменён». Горячая клавиша закреплена за Save-действием.
void GhostSuggestionTest::documentSaveWritesUtf8AndClearsModified()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("save.txt"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly)); // пустой — есть что править
    }

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* editor =
        window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
    auto* indicator =
        window.findChild<QLabel*>(QStringLiteral("documentIndicator"));
    QVERIFY(editor && indicator);

    auto* saveAction =
        window.findChild<QAction*>(QStringLiteral("actionSave"));
    QVERIFY(saveAction);
    // Горячая клавиша сохранения: Ctrl+S (Cmd+S на macOS).
    QCOMPARE(saveAction->shortcut(), QKeySequence::Save);

    QVERIFY(activateAndFocus(window, *editor));

    QVERIFY(window.openFile(path));
    const QString typed =
        QStringLiteral("Новый русский текст: сохраняется ёЁ.");
    // QTest::keyClicks падает на не-ASCII (таблица Qt Test — только
    // латиница); вставка напрямую даёт те же сигналы документа.
    editor->insertPlainText(typed);
    QCOMPARE(editor->toPlainText(), typed);
    QVERIFY(editor->document()->isModified());
    QVERIFY(indicator->text().contains(QStringLiteral(" • изменён")));
    QVERIFY(window.isWindowModified());

    saveAction->trigger();

    // Байты файла — ровно UTF-8 русского текста (без BOM).
    QFile saved(path);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), typed.toUtf8());
    QVERIFY(!typed.toUtf8().startsWith("\xEF\xBB\xBF"));

    QVERIFY(!editor->document()->isModified());
    QVERIFY(!window.isWindowModified());
    QVERIFY(indicator->text().contains(QStringLiteral(" • не изменён")));
    QVERIFY(window.statusBar()->currentMessage().contains(
        QStringLiteral("Сохранено")));
}

// Ctrl+N (actionNew): редактор пуст, «Без имени» (путь сброшен),
// modified сброшен, показанная подсказка очищена, генерация не
// стартует. Клавиши остальных команд документа тоже закреплены.
void GhostSuggestionTest::documentNewClearsEditorAndSuggestion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("new.txt"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* editor =
        window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
    auto* controller = window.findChild<SuggestionController*>();
    auto* overlay = window.findChild<SuggestionOverlay*>();
    auto* indicator =
        window.findChild<QLabel*>(QStringLiteral("documentIndicator"));
    QVERIFY(editor && controller && overlay && indicator);

    // Все команды документа привязаны к своим горячим клавишам.
    auto* newAction =
        window.findChild<QAction*>(QStringLiteral("actionNew"));
    auto* openAction =
        window.findChild<QAction*>(QStringLiteral("actionOpen"));
    auto* saveAsAction =
        window.findChild<QAction*>(QStringLiteral("actionSaveAs"));
    QVERIFY(newAction && openAction && saveAsAction);
    QCOMPARE(newAction->shortcut(), QKeySequence::New);
    QCOMPARE(openAction->shortcut(), QKeySequence::Open);
    QCOMPARE(saveAsAction->shortcut(), QKeySequence::SaveAs);

    QVERIFY(activateAndFocus(window, *editor));

    // Был открыт файл и показана подсказка.
    QVERIFY(window.openFile(path));
    // Не-ASCII — вставка напрямую (keyClicks падает на кириллице).
    editor->insertPlainText(QStringLiteral("лишний текст"));
    QVERIFY(QTest::qWaitFor([&]() { return controller->hasSuggestion(); },
                            5000));
    QVERIFY(overlay->isVisible());
    editor->document()->setModified(false); // без запроса maybeSave (см. выше)

    newAction->trigger();

    QVERIFY(editor->toPlainText().isEmpty());
    QVERIFY(!editor->document()->isModified());
    QVERIFY(!window.isWindowModified());
    QVERIFY(indicator->text().contains(QStringLiteral("Без имени")));
    QVERIFY(window.windowTitle().contains(QStringLiteral("Без имени")));
    QVERIFY(!controller->hasSuggestion());
    QVERIFY(!overlay->isVisible());
    QCOMPARE(controller->state(), SuggestionController::State::Idle);

    // Генерация после «Нового» не стартует сама.
    QTest::qWait(700);
    QCOMPARE(controller->state(), SuggestionController::State::Idle);
    QVERIFY(!controller->hasSuggestion());
}

// Close event с запросом о несохранённых изменениях — три решения:
//  1) Cancel: окно остаётся открытым, shutdown НЕ выполнялся —
//     контроллер жив, подсказка после отмены всё ещё работает;
//  2) Discard: окно закрывается, файл не тронут;
//  3) Save: окно закрывается, правки записаны (путь уже известен —
//     без диалога выбора файла).
void GhostSuggestionTest::closeEventPromptsForUnsavedChanges()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("close.txt"));
    const QByteArray original =
        QStringLiteral("старое содержимое").toUtf8();
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(original), original.size());
    }

    const auto readFile = [&path]() {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    };

    // --- (1) Cancel: остаёмся в окне, контроллер жив -------------------
    {
        MainWindow window;
        window.resize(1000, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* editor =
            window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
        auto* controller = window.findChild<SuggestionController*>();
        QVERIFY(editor && controller);

        QVERIFY(activateAndFocus(window, *editor));

        QVERIFY(window.openFile(path));
        editor->insertPlainText(QStringLiteral(" и правка"));
        QVERIFY(editor->document()->isModified());

        QTimer::singleShot(0, &window, [&window]() {
            auto* box = window.findChild<QMessageBox*>();
            if (box != nullptr) {
                QAbstractButton* cancel =
                    box->button(QMessageBox::Cancel);
                if (cancel != nullptr)
                    cancel->click();
            }
        });
        window.close();

        QVERIFY2(window.isVisible(),
                 "Cancel должен оставить окно открытым");
        QVERIFY(window.isWindowModified());
        QVERIFY2(controller->isEnabled(),
                 "После отмены closeEvent не должен останавливать "
                 "контроллер (shutdown не выполнялся)");
        QVERIFY(!controller->hasSuggestion());

        // Подсказки работают после отмены: контроллер не отвязан.
        QVERIFY(activateAndFocus(window, *editor));
        editor->insertPlainText(QStringLiteral(" ещё"));
        QVERIFY(QTest::qWaitFor(
            [&]() { return controller->hasSuggestion(); }, 5000));
    }
    // Ни одно из решений не записало файл — Cancel тем более.
    QCOMPARE(readFile(), original);

    // --- (2) Discard: закрываемся, файл не тронут ----------------------
    {
        MainWindow window;
        window.resize(1000, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* editor =
            window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
        QVERIFY(editor);
        QVERIFY(window.openFile(path));
        editor->insertPlainText(QStringLiteral(" отменённая правка"));
        QVERIFY(editor->document()->isModified());

        QTimer::singleShot(0, &window, [&window]() {
            auto* box = window.findChild<QMessageBox*>();
            if (box != nullptr) {
                QAbstractButton* discard =
                    box->button(QMessageBox::Discard);
                if (discard != nullptr)
                    discard->click();
            }
        });
        window.close();

        QVERIFY(!window.isVisible());
        QCOMPARE(readFile(), original);
    }

    // --- (3) Save: закрываемся, правки в файле -------------------------
    {
        MainWindow window;
        window.resize(1000, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* editor =
            window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
        QVERIFY(editor);

        QVERIFY(window.openFile(path));
        QTextCursor cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        editor->setTextCursor(cursor);
        const QString typed = QStringLiteral(" и сохранённая правка");
        editor->insertPlainText(typed);
        QVERIFY(editor->document()->isModified());

        QTimer::singleShot(0, &window, [&window]() {
            auto* box = window.findChild<QMessageBox*>();
            if (box != nullptr) {
                QAbstractButton* save =
                    box->button(QMessageBox::Save);
                if (save != nullptr)
                    save->click();
            }
        });
        window.close();

        QVERIFY(!window.isVisible());
        QCOMPARE(readFile(), original + typed.toUtf8());
    }
}

// Кнопка Settings в панели -> диалог -> применение настроек: контроллер
// получает новые значения сразу, QSettings сохраняет их (переживут
// перезапуск). Модельные настройки не менялись — llama-поток и активная
// генерация не затрагиваются.
void GhostSuggestionTest::settingsDialogAppliesAndPersists()
{
    MainWindow window;

    auto* settingsButton = window.findChild<QPushButton*>(
        QStringLiteral("settingsButton"));
    QVERIFY2(settingsButton, "Кнопка Settings не создана");

    bool dialogTouched = false;
    // click() -> openSettings() -> dialog.exec() (вложенный event
    // loop): таймер срабатывает внутри exec и закрывает диалог.
    QTimer::singleShot(0, &window, [&]() {
        auto* dialog = window.findChild<SettingsDialog*>(
            QStringLiteral("settingsDialog"));
        if (dialog == nullptr)
            return;
        auto* debounce = dialog->findChild<QSpinBox*>(
            QStringLiteral("debounceEdit"));
        auto* autoCheck = dialog->findChild<QCheckBox*>(
            QStringLiteral("autoSuggestionsCheck"));
        if (debounce == nullptr || autoCheck == nullptr)
            return;
        debounce->setValue(750);
        autoCheck->setChecked(false);
        dialogTouched = true;
        dialog->accept();
    });

    settingsButton->click(); // модальный exec до accept()
    QVERIFY2(dialogTouched, "Диалог настроек не открылся");

    // Применено контроллеру — сразу, без пересоздания backend'а.
    auto* controller = window.findChild<SuggestionController*>();
    QVERIFY(controller);
    QCOMPARE(controller->debounceInterval(), 750);
    QCOMPARE(controller->autoSuggestions(), false);

    // И сохранено в QSettings — переживёт перезапуск приложения.
    const AppSettings loaded = AppSettings::load();
    QCOMPARE(loaded.debounceMs, 750);
    QCOMPARE(loaded.autoSuggestions, false);

    // Дефолты обратно: последующие тесты не зависят от порядка
    // (cleanupTestCase вернёт пользовательские значения).
    QSettings settings(QStringLiteral("PtuchAI"),
                       QStringLiteral("PtuchEditor"));
    settings.setValue(AppSettings::keyDebounceMs, 500);
    settings.setValue(AppSettings::keyAutoSuggestions, true);
    settings.sync();
}

// StylePanel в MainWindow подключён к контроллеру: изменение микса
// чистит показанную ghost-подсказку (setStyleMix -> clearSuggestion),
// нормализованные веса доходят до контроллера, и контроллер
// продолжает работать — перепланированный запрос приносит новую
// подсказку уже с новым миксом.
void GhostSuggestionTest::styleMixChangeClearsGhostSuggestion()
{
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* editor =
        window.findChild<QPlainTextEdit*>(QStringLiteral("mainEditor"));
    auto* controller = window.findChild<SuggestionController*>();
    auto* overlay = window.findChild<SuggestionOverlay*>();
    QVERIFY(editor && controller && overlay);

    // Панель стилей — в отдельном dock (модель — внутри панели).
    auto* panel =
        window.findChild<StylePanel*>(QStringLiteral("stylePanel"));
    QVERIFY2(panel, "StylePanel не создан в MainWindow");
    auto* dock = window.findChild<QDockWidget*>(
        QStringLiteral("styleDock"));
    QVERIFY2(dock, "QDockWidget стилей не создан");

    // Фокус обязателен: без него контроллер не стартует debounce.
    QVERIFY(activateAndFocus(window, *editor));

    // Печать -> debounce (500 мс) -> mock (~300 мс) -> ghost показан.
    QTest::keyClicks(editor, QStringLiteral("hello"));
    QVERIFY(QTest::qWaitFor([&]() { return controller->hasSuggestion(); },
                            5000));
    QVERIFY(overlay->isVisible());

    // Меняем микс: слайдер первого профиля.
    auto* slider =
        panel->findChild<QSlider*>(QStringLiteral("slider_pushkin"));
    QVERIFY(slider);
    slider->setValue(60);

    // Требование: текущая ghost очищена немедленно, документ цел.
    QVERIFY2(!controller->hasSuggestion(),
             "Смена микса должна очистить ghost-подсказку");
    QVERIFY(!overlay->isVisible());
    QCOMPARE(editor->toPlainText(), QStringLiteral("hello"));

    // Нормализованные веса дошли до контроллера (считает модель панели).
    const QVector<StyleWeight>& mix = controller->styles();
    QCOMPARE(mix.size(), 1);
    QCOMPARE(mix.first().styleId, QStringLiteral("pushkin"));
    QVERIFY(qFuzzyCompare(double(mix.first().weight), 1.0));

    // Контроллер жив: перепланированный setStyleMix-ом запрос приносит
    // свежую подсказку (микс применён, дебаунс перезапущен).
    QVERIFY(QTest::qWaitFor([&]() { return controller->hasSuggestion(); },
                            5000));
    QVERIFY(overlay->isVisible());
    QCOMPARE(editor->toPlainText(), QStringLiteral("hello"));
}

void GhostSuggestionTest::diagnosticsButtonOpensDialog()
{
#if PTUCH_DIAGNOSTICS
    MainWindow window;

    auto* button = window.findChild<QPushButton*>(
        QStringLiteral("diagnosticsButton"));
    QVERIFY2(button, "Кнопка Diagnostics не создана");

    QString backendText;
    QString modelText;
    QString copiedReport;
    // click() -> openDiagnostics() -> dialog.exec() (вложенный event
    // loop): таймер срабатывает внутри exec — читает метки, жмёт
    // «Копировать» и закрывает диалог.
    QTimer::singleShot(0, &window, [&]() {
        auto* dialog = window.findChild<DiagnosticsDialog*>(
            QStringLiteral("diagnosticsDialog"));
        if (dialog == nullptr)
            return;
        auto* backendLabel = dialog->findChild<QLabel*>(
            QStringLiteral("backendValue"));
        auto* modelLabel = dialog->findChild<QLabel*>(
            QStringLiteral("modelValue"));
        auto* copy = dialog->findChild<QPushButton*>(
            QStringLiteral("copyButton"));
        if (backendLabel == nullptr || modelLabel == nullptr
            || copy == nullptr)
            return;
        backendText = backendLabel->text();
        modelText = modelLabel->text();
        copy->click();
        copiedReport = QGuiApplication::clipboard()->text();
        dialog->accept();
    });

    button->click(); // модальный exec до accept()
    QVERIFY2(!backendText.isEmpty(), "Диагностика не открылась");

    // Активный backend в этих тестах — mock (llama отключён через
    // PTUCH_DISABLE_LLAMA): имя модели отсутствует («—»).
    QCOMPARE(backendText, QStringLiteral("mock"));
    QCOMPARE(modelText, QStringLiteral("—"));
    QVERIFY2(copiedReport.contains(QStringLiteral("mock")),
             "Отчёт не скопирован в буфер обмена");
    QVERIFY2(!copiedReport.contains(QStringLiteral("0x")),
             "Отчёт не должен содержать адреса");
#else
    QSKIP("Режим диагностики выключен (PTUCH_DIAGNOSTICS=OFF)");
#endif
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
