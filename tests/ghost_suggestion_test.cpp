// ghost_suggestion_test.cpp
#include <QtTest>

#include "UI/MainWindow.h"
#include "UI/settings_dialog.h"
#include "UI/style_panel.h"
#include "UI/suggestion_overlay.h"
#include "llama/llama_backend.h"
#include "settings/app_settings.h"
#include "suggestion/suggestion_controller.h"

#include <QCheckBox>
#include <QDockWidget>
#include <QFontMetrics>
#include <QImage>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
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
           key.startsWith(QLatin1String("suggestion/"));
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
    void keysScenario();
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
    window.activateWindow();
    editor->setFocus();
    QVERIFY(QTest::qWaitFor([&]() { return editor->hasFocus(); }, 3000));

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
