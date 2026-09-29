// settings_test.cpp
#include <QtTest>

#include "UI/settings_dialog.h"
#include "llama/llama_backend.h"
#include "settings/app_settings.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>

#if PTUCH_DIAGNOSTICS
// Режим диагностики: диалог снимка и его тесты (слоты ниже при
// выключенной опции QSKIP и не компилируются в этот файл).
#include "UI/diagnostics_dialog.h"
#include <QClipboard>
#include <QGuiApplication>
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

} // namespace

// ---------------------------------------------------------------------------
// Настройки: дефолты, сохранение между запусками, санитизация диапазонов
// (некорректные значения не доезжают до LlamaBackend/контроллера) и
// поведение SettingsDialog: заполнение, жёсткие диапазоны, отображение
// ошибки Test Model.
//
// Тесты пишут в ТОТ ЖЕ файл QSettings, что и приложение
// (PtuchAI/PtuchEditor): init снимает и удаляет только наши ключи,
// cleanup возвращает пользовательские значения.
// ---------------------------------------------------------------------------
class SettingsTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void defaultsAreUsedWithoutStoredValues();
    void savedValuesRoundtripAcrossLaunches();
    void outOfRangeStoredValuesAreSanitized();
    void dialogShowsStoredSettings();
    void dialogWidgetsEnforceRanges();
    void dialogReturnsValuesInRange();
    void testModelReportsMissingFileWithoutBlocking();
    void testModelReportsCorruptGguf();
    void testModelRealGgufLoads(); // gated: PTUCH_MODEL_TESTS=1

    // Диагностика backend'а (слоты сами QSKIP при PTUCH_DIAGNOSTICS=0).
    void diagnosticsReportFormatsSnapshot();
    void diagnosticsDialogShowsAndCopiesReport();

private:
    static QSettings makeSettings()
    {
        return QSettings(QStringLiteral("PtuchAI"),
                         QStringLiteral("PtuchEditor"));
    }

    QVariantMap m_original;
};

void SettingsTest::init()
{
    // Окружение не должно перенаправить поиск модели в тестах.
    qunsetenv("PTUCH_MODEL_PATH");
    qunsetenv("PTUCH_MODEL_DIR");

    // Снимок наших ключей + дефолтное состояние теста (чужие ключи
    // файла — в т.ч. fallback NSGlobalDomain — не трогаем).
    QSettings settings = makeSettings();
    const QStringList keys = settings.allKeys();
    for (const QString& key : keys) {
        if (!isOurSettingsKey(key))
            continue;
        m_original.insert(key, settings.value(key));
        settings.remove(key);
    }
    settings.sync();
}

void SettingsTest::cleanup()
{
    // Удаляем наши ключи (в т.ч. мусор тестов) и возвращаем
    // пользовательские значения дословно.
    QSettings settings = makeSettings();
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

void SettingsTest::defaultsAreUsedWithoutStoredValues()
{
    // Ключей нет -> дефолты структуры (файл очищен в init).
    const AppSettings value = AppSettings::load();
    const AppSettings defaults;

    QVERIFY(value.modelPath.isEmpty());
    QCOMPARE(value.contextSize, 4096);
    QCOMPARE(value.maxTokens, 64);
    QCOMPARE(value.temperature, 0.7);
    QCOMPARE(value.topP, 0.9);
    QCOMPARE(value.gpuLayers, -1);
    QCOMPARE(value.debounceMs, 500);
    QVERIFY(value.autoSuggestions);

    // Дефолты обязаны лежать внутри допустимых диапазонов —
    // иначе sanitize() поломала бы их же.
    QVERIFY(defaults.contextSize >= AppSettings::contextSizeMin &&
            defaults.contextSize <= AppSettings::contextSizeMax);
    QVERIFY(defaults.maxTokens >= AppSettings::maxTokensMin &&
            defaults.maxTokens <= AppSettings::maxTokensMax);
    QVERIFY(defaults.debounceMs >= AppSettings::debounceMin &&
            defaults.debounceMs <= AppSettings::debounceMax);
}

void SettingsTest::savedValuesRoundtripAcrossLaunches()
{
    AppSettings value;
    value.contextSize = 1024;
    value.maxTokens = 32;
    value.temperature = 1.5;
    value.topP = 0.95;
    value.gpuLayers = 12;
    value.debounceMs = 250;
    value.autoSuggestions = false;
    value.save();

    // «Новый запуск» = новое чтение из QSettings.
    const AppSettings loaded = AppSettings::load();
    QCOMPARE(loaded.contextSize, value.contextSize);
    QCOMPARE(loaded.maxTokens, value.maxTokens);
    QCOMPARE(loaded.temperature, value.temperature);
    QCOMPARE(loaded.topP, value.topP);
    QCOMPARE(loaded.gpuLayers, value.gpuLayers);
    QCOMPARE(loaded.debounceMs, value.debounceMs);
    QCOMPARE(loaded.autoSuggestions, value.autoSuggestions);

    // Ключ llama/modelPath общий с resolveModelPath(): сохранённый путь
    // приложение находит при поиске модели (файл создаём реально).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString model = dir.filePath(QStringLiteral("model.gguf"));
    {
        QFile file(model);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("GGUF-fake");
    }
    AppSettings withPath;
    withPath.modelPath = model;
    withPath.save();
    QCOMPARE(resolveModelPath(), QFileInfo(model).absoluteFilePath());
}

void SettingsTest::outOfRangeStoredValuesAreSanitized()
{
    // Правленый руками файл настроек: мусор и значения за границами.
    QSettings settings = makeSettings();
    settings.setValue(AppSettings::keyModelPath, QStringLiteral(""));
    settings.setValue(AppSettings::keyContextSize, 999999);
    settings.setValue(AppSettings::keyMaxTokens, -5);
    settings.setValue(AppSettings::keyTemperature,
                      QStringLiteral("abc"));
    settings.setValue(AppSettings::keyTopP, 42.0);
    settings.setValue(AppSettings::keyGpuLayers, 777);
    settings.setValue(AppSettings::keyDebounceMs, -1);
    settings.sync();

    // load() обязан вернуть валидные значения: именно они уходят в
    // LlamaBackend и контроллер — некорректное не проходит.
    const AppSettings value = AppSettings::load();
    QCOMPARE(value.contextSize, AppSettings::contextSizeMax);
    QCOMPARE(value.maxTokens, AppSettings::maxTokensMin);
    // "abc" -> 0.0 -> нижняя граница диапазона.
    QCOMPARE(value.temperature, AppSettings::temperatureMin);
    QCOMPARE(value.topP, AppSettings::topPMax);
    QCOMPARE(value.gpuLayers, AppSettings::gpuLayersMax);
    QCOMPARE(value.debounceMs, AppSettings::debounceMin);
}

void SettingsTest::dialogShowsStoredSettings()
{
    AppSettings stored;
    stored.modelPath = QStringLiteral("/models/custom.gguf");
    stored.contextSize = 2048;
    stored.maxTokens = 128;
    stored.temperature = 1.25;
    stored.topP = 0.85;
    stored.gpuLayers = 24;
    stored.debounceMs = 750;
    stored.autoSuggestions = false;
    stored.save();

    SettingsDialog dialog;

    auto* path = dialog.findChild<QLineEdit*>(
        QStringLiteral("modelPathEdit"));
    auto* context = dialog.findChild<QSpinBox*>(
        QStringLiteral("contextSizeEdit"));
    auto* maxTokens = dialog.findChild<QSpinBox*>(
        QStringLiteral("maxTokensEdit"));
    auto* temperature = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("temperatureEdit"));
    auto* topP = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("topPEdit"));
    auto* gpuLayers = dialog.findChild<QSpinBox*>(
        QStringLiteral("gpuLayersEdit"));
    auto* debounce = dialog.findChild<QSpinBox*>(
        QStringLiteral("debounceEdit"));
    auto* autoCheck = dialog.findChild<QCheckBox*>(
        QStringLiteral("autoSuggestionsCheck"));
    auto* testButton = dialog.findChild<QPushButton*>(
        QStringLiteral("testModelButton"));
    auto* statusLabel = dialog.findChild<QLabel*>(
        QStringLiteral("testStatusLabel"));

    QVERIFY(path && context && maxTokens && temperature && topP);
    QVERIFY(gpuLayers && debounce && autoCheck);
    QVERIFY(testButton && statusLabel);

    QCOMPARE(path->text(), stored.modelPath);
    QCOMPARE(context->value(), stored.contextSize);
    QCOMPARE(maxTokens->value(), stored.maxTokens);
    QCOMPARE(temperature->value(), stored.temperature);
    QCOMPARE(topP->value(), stored.topP);
    QCOMPARE(gpuLayers->value(), stored.gpuLayers);
    QCOMPARE(debounce->value(), stored.debounceMs);
    QCOMPARE(autoCheck->isChecked(), stored.autoSuggestions);
}

void SettingsTest::dialogWidgetsEnforceRanges()
{
    SettingsDialog dialog;

    // Числовые поля ограничены диапазонами AppSettings: выход за
    // границы полем невозможен, setValue сверх максимума клампится.
    auto* context = dialog.findChild<QSpinBox*>(
        QStringLiteral("contextSizeEdit"));
    auto* maxTokens = dialog.findChild<QSpinBox*>(
        QStringLiteral("maxTokensEdit"));
    auto* temperature = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("temperatureEdit"));
    auto* topP = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("topPEdit"));
    auto* gpuLayers = dialog.findChild<QSpinBox*>(
        QStringLiteral("gpuLayersEdit"));
    auto* debounce = dialog.findChild<QSpinBox*>(
        QStringLiteral("debounceEdit"));
    QVERIFY(context && maxTokens && temperature && topP);
    QVERIFY(gpuLayers && debounce);

    QCOMPARE(context->minimum(), AppSettings::contextSizeMin);
    QCOMPARE(context->maximum(), AppSettings::contextSizeMax);
    QCOMPARE(maxTokens->minimum(), AppSettings::maxTokensMin);
    QCOMPARE(maxTokens->maximum(), AppSettings::maxTokensMax);
    QCOMPARE(temperature->minimum(), AppSettings::temperatureMin);
    QCOMPARE(temperature->maximum(), AppSettings::temperatureMax);
    QCOMPARE(topP->minimum(), AppSettings::topPMin);
    QCOMPARE(topP->maximum(), AppSettings::topPMax);
    QCOMPARE(gpuLayers->minimum(), AppSettings::gpuLayersMin);
    QCOMPARE(gpuLayers->maximum(), AppSettings::gpuLayersMax);
    QCOMPARE(debounce->minimum(), AppSettings::debounceMin);
    QCOMPARE(debounce->maximum(), AppSettings::debounceMax);

    // Попытка выйти за границы -> кламп к максимуму/минимуму.
    context->setValue(AppSettings::contextSizeMax + 100000);
    QCOMPARE(context->value(), AppSettings::contextSizeMax);
    maxTokens->setValue(-42);
    QCOMPARE(maxTokens->value(), AppSettings::maxTokensMin);
    temperature->setValue(99.0);
    QCOMPARE(temperature->value(), AppSettings::temperatureMax);
    debounce->setValue(-1);
    QCOMPARE(debounce->value(), AppSettings::debounceMin);
}

void SettingsTest::dialogReturnsValuesInRange()
{
    SettingsDialog dialog;

    auto* path = dialog.findChild<QLineEdit*>(
        QStringLiteral("modelPathEdit"));
    QVERIFY(path);
    // Случайные пробелы вокруг пути отбрасываются.
    path->setText(QStringLiteral("   /tmp/model.gguf   "));

    const AppSettings value = dialog.settings();
    QCOMPARE(value.modelPath, QStringLiteral("/tmp/model.gguf"));

    // Любые значения из settings() в допустимых диапазонах —
    // то, что уйдёт в QSettings и (через load) в LlamaBackend.
    QVERIFY(value.contextSize >= AppSettings::contextSizeMin &&
            value.contextSize <= AppSettings::contextSizeMax);
    QVERIFY(value.maxTokens >= AppSettings::maxTokensMin &&
            value.maxTokens <= AppSettings::maxTokensMax);
    QVERIFY(value.temperature >= AppSettings::temperatureMin &&
            value.temperature <= AppSettings::temperatureMax);
    QVERIFY(value.topP >= AppSettings::topPMin &&
            value.topP <= AppSettings::topPMax);
    QVERIFY(value.gpuLayers >= AppSettings::gpuLayersMin &&
            value.gpuLayers <= AppSettings::gpuLayersMax);
    QVERIFY(value.debounceMs >= AppSettings::debounceMin &&
            value.debounceMs <= AppSettings::debounceMax);
}

void SettingsTest::testModelReportsMissingFileWithoutBlocking()
{
    SettingsDialog dialog;

    auto* path = dialog.findChild<QLineEdit*>(
        QStringLiteral("modelPathEdit"));
    auto* testButton = dialog.findChild<QPushButton*>(
        QStringLiteral("testModelButton"));
    auto* statusLabel = dialog.findChild<QLabel*>(
        QStringLiteral("testStatusLabel"));
    QVERIFY(path && testButton && statusLabel);

    // Несуществующий файл: ошибка показывается сразу, без создания
    // потока и без зависания UI.
    path->setText(
        QStringLiteral("/definitely/missing/model.gguf"));
    testButton->click();

    QVERIFY(statusLabel->text().contains(
        QStringLiteral("Файл не найден")));
    QVERIFY(testButton->isEnabled()); // кнопка снова доступна

    // Повторный клик не должен ничего сломать.
    testButton->click();
    QVERIFY(statusLabel->text().contains(
        QStringLiteral("Файл не найден")));
}

void SettingsTest::testModelReportsCorruptGguf()
{
    // Файл существует, но не является GGUF: ошибка приходит асинхронно
    // из тест-потока и отображается меткой; UI не блокируется.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString corrupt = dir.filePath(QStringLiteral("broken.gguf"));
    {
        QFile file(corrupt);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("THIS IS NOT A GGUF MODEL");
    }

    SettingsDialog dialog;

    auto* path = dialog.findChild<QLineEdit*>(
        QStringLiteral("modelPathEdit"));
    auto* testButton = dialog.findChild<QPushButton*>(
        QStringLiteral("testModelButton"));
    auto* statusLabel = dialog.findChild<QLabel*>(
        QStringLiteral("testStatusLabel"));
    QVERIFY(path && testButton && statusLabel);

    path->setText(corrupt);
    testButton->click();

    // Пока грузится — кнопка заблокирована (защита от параллельных
    // тестов); результат придёт из worker-потока.
    QVERIFY(QTest::qWaitFor(
        [&]() {
            return statusLabel->text().contains(
                QStringLiteral("Ошибка загрузки"));
        },
        20000));

    // После ошибки тест-поток остановлен, кнопка возвращена.
    QVERIFY(QTest::qWaitFor([&]() { return testButton->isEnabled(); },
                            5000));
    QVERIFY(statusLabel->text().contains(corrupt) ||
            statusLabel->text().contains(
                QStringLiteral("Ошибка загрузки")));
}

void SettingsTest::testModelRealGgufLoads()
{
    // Полный сценарий Test Model с реальной моделью (2 ГБ):
    // включается PTUCH_MODEL_TESTS=1, в ctest по умолчанию пропускается.
    if (!qEnvironmentVariableIsSet("PTUCH_MODEL_TESTS"))
        QSKIP("Тест с реальной GGUF включается PTUCH_MODEL_TESTS=1");

    const QString model = resolveModelPath();
    if (model.isEmpty())
        QSKIP("GGUF-модель не найдена (models/ или настройки)");

    SettingsDialog dialog;

    auto* path = dialog.findChild<QLineEdit*>(
        QStringLiteral("modelPathEdit"));
    auto* testButton = dialog.findChild<QPushButton*>(
        QStringLiteral("testModelButton"));
    auto* statusLabel = dialog.findChild<QLabel*>(
        QStringLiteral("testStatusLabel"));
    QVERIFY(path && testButton && statusLabel);

    path->setText(model);
    testButton->click();

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return statusLabel->text().startsWith(
                         QStringLiteral("OK:"));
                 },
                 120000),
             "Модель не загрузилась за 120 с");

    // Деструктор диалога останавливает тест-поток (RAII) — падение
    // здесь из-за зависшего потока завалит тест.
}

void SettingsTest::diagnosticsReportFormatsSnapshot()
{
#if PTUCH_DIAGNOSTICS
    // Полный снимок: каждое поле попадает в отчёт (включая
    // посчитанные «токенов в секунду»), плюс проверка, что отчёт
    // строится только из полей снимка — без указателей и адресов.
    BackendDiagnostics data;
    data.backendName = QStringLiteral("llama.cpp");
    data.modelName = QStringLiteral("test-model.gguf");
    data.contextSize = 4096;
    data.gpuLayers = -1;
    data.promptTokens = 128;
    data.generatedTokens = 64;
    data.promptProcessingMs = 250;
    data.generationMs = 3000;
    data.lastError = QStringLiteral("последняя ошибка");

    const QString report = DiagnosticsDialog::formatReport(data);
    QVERIFY(report.contains(QStringLiteral("llama.cpp")));
    QVERIFY(report.contains(QStringLiteral("test-model.gguf")));
    QVERIFY(report.contains(QStringLiteral("4096")));
    QVERIFY(report.contains(QStringLiteral("-1"))); // все слои
    QVERIFY(report.contains(QStringLiteral("128")));
    QVERIFY(report.contains(QStringLiteral("64")));
    QVERIFY(report.contains(QStringLiteral("250")));
    QVERIFY(report.contains(QStringLiteral("3000")));
    // 64 токена за 3000 мс = 21.3 т/с.
    QVERIFY(report.contains(QStringLiteral("21.3")));
    QVERIFY(report.contains(QStringLiteral("последняя ошибка")));

    // Необработанные указатели/внутренние адреса запрещены.
    QVERIFY2(!report.contains(QStringLiteral("0x")),
             "Отчёт диагностики не должен содержать адреса");

    // Пустой снимок (mock / модель не загружена): поля «—», ошибок нет.
    const QString empty = DiagnosticsDialog::formatReport(
        BackendDiagnostics {});
    QVERIFY(empty.contains(QStringLiteral("—")));
    QVERIFY(empty.contains(QStringLiteral("нет ошибок")));
#else
    QSKIP("Режим диагностики выключен (PTUCH_DIAGNOSTICS=OFF)");
#endif
}

void SettingsTest::diagnosticsDialogShowsAndCopiesReport()
{
#if PTUCH_DIAGNOSTICS
    BackendDiagnostics data;
    data.backendName = QStringLiteral("llama.cpp");
    data.modelName = QStringLiteral("model.gguf");
    data.contextSize = 2048;
    data.gpuLayers = 20;
    data.promptTokens = 10;
    data.generatedTokens = 20;
    data.promptProcessingMs = 50;
    data.generationMs = 1000;
    data.lastError = QStringLiteral("текст ошибки здесь");

    DiagnosticsDialog dialog(data);
    QCOMPARE(dialog.objectName(), QStringLiteral("diagnosticsDialog"));

    // Каждое поле снимка — на своей метке (имена объектов стабильны).
    const auto value = [&dialog](const char* name) {
        auto* label =
            dialog.findChild<QLabel*>(QString::fromLatin1(name));
        return label != nullptr ? label->text() : QString();
    };
    QCOMPARE(value("backendValue"), QStringLiteral("llama.cpp"));
    QCOMPARE(value("modelValue"), QStringLiteral("model.gguf"));
    QCOMPARE(value("contextValue"), QStringLiteral("2048"));
    QCOMPARE(value("gpuLayersValue"), QStringLiteral("20"));
    QCOMPARE(value("promptTokensValue"), QStringLiteral("10"));
    QCOMPARE(value("generatedTokensValue"), QStringLiteral("20"));
    QCOMPARE(value("promptTimeValue"), QStringLiteral("50 мс"));
    QCOMPARE(value("generationTimeValue"), QStringLiteral("1000 мс"));
    // 20 токенов за 1000 мс = 20.0 т/с.
    QCOMPARE(value("tpsValue"), QStringLiteral("20.0"));
    QVERIFY(value("errorValue").contains(
        QStringLiteral("текст ошибки здесь")));

    // Кнопка копирования кладёт отчёт в буфер обмена — и только его
    // поля (без адресов).
    auto* copy = dialog.findChild<QPushButton*>(
        QStringLiteral("copyButton"));
    QVERIFY2(copy, "Кнопка Копировать не создана");
    copy->click();
    const QString clipboard = QGuiApplication::clipboard()->text();
    QVERIFY2(clipboard.contains(QStringLiteral("model.gguf")),
             "Отчёт не скопирован в буфер обмена");
    QVERIFY(clipboard.contains(QStringLiteral("20.0")));
    QVERIFY(!clipboard.contains(QStringLiteral("0x")));
#else
    QSKIP("Режим диагностики выключен (PTUCH_DIAGNOSTICS=OFF)");
#endif
}

QTEST_MAIN(SettingsTest)

#include "settings_test.moc"
