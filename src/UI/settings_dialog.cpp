// settings_dialog.cpp
#include "settings_dialog.h"

#include "llama/llama_backend.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("Настройки"));
    setModal(true);

    // Тема приложения распространяется на диалог (наследование
    // от MainWindow); собственный stylesheet задаёт фон и состояния
    // контролов (hover/pressed/disabled/focus) — по схеме из настроек
    // (default — пустой stylesheet, системный вид контролов).
    const AppSettings values = AppSettings::load();
    m_scheme = Theme::schemeFromId(values.style);
    setStyleSheet(Theme::dialogStyleSheet(m_scheme));

    auto* mainLayout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setSpacing(8);
    form->setContentsMargins(0, 0, 0, 0);

    // --- Путь к GGUF-модели: строка + кнопка выбора файла.
    m_modelPathEdit = new QLineEdit(this);
    m_modelPathEdit->setObjectName(QStringLiteral("modelPathEdit"));
    m_modelPathEdit->setText(values.modelPath);
    m_modelPathEdit->setPlaceholderText(
        tr("не задан — авто-поиск (models/, переменные окружения)"));
    // Ширина полей одинакова для всех схем: без stylesheet'ов
    // (default) sizeHint поля слишком узок для чтения пути/значений.
    m_modelPathEdit->setMinimumWidth(340);

    m_browseButton = new QPushButton(QStringLiteral("…"), this);
    m_browseButton->setObjectName(QStringLiteral("browseModelButton"));
    m_browseButton->setToolTip(tr("Выбрать файл GGUF..."));
    connect(m_browseButton, &QPushButton::clicked,
            this, &SettingsDialog::browseModelPath);

    auto* pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->addWidget(m_modelPathEdit, 1);
    pathRow->addWidget(m_browseButton);
    form->addRow(tr("Модель (GGUF):"), pathRow);

    // --- Размер контекста.
    m_contextSizeEdit = new QSpinBox(this);
    m_contextSizeEdit->setObjectName(QStringLiteral("contextSizeEdit"));
    m_contextSizeEdit->setRange(AppSettings::contextSizeMin,
                                AppSettings::contextSizeMax);
    m_contextSizeEdit->setSingleStep(256);
    m_contextSizeEdit->setValue(values.contextSize);
    m_contextSizeEdit->setMinimumWidth(110);
    form->addRow(tr("Размер контекста:"), m_contextSizeEdit);

    // --- Максимум новых токенов.
    m_maxTokensEdit = new QSpinBox(this);
    m_maxTokensEdit->setObjectName(QStringLiteral("maxTokensEdit"));
    m_maxTokensEdit->setRange(AppSettings::maxTokensMin,
                              AppSettings::maxTokensMax);
    m_maxTokensEdit->setSingleStep(8);
    m_maxTokensEdit->setValue(values.maxTokens);
    m_maxTokensEdit->setMinimumWidth(110);
    form->addRow(tr("Макс. новых токенов:"), m_maxTokensEdit);

    // --- Temperature.
    m_temperatureEdit = new QDoubleSpinBox(this);
    m_temperatureEdit->setObjectName(QStringLiteral("temperatureEdit"));
    m_temperatureEdit->setRange(AppSettings::temperatureMin,
                                AppSettings::temperatureMax);
    m_temperatureEdit->setDecimals(2);
    m_temperatureEdit->setSingleStep(0.05);
    m_temperatureEdit->setValue(values.temperature);
    m_temperatureEdit->setMinimumWidth(110);
    form->addRow(tr("Temperature:"), m_temperatureEdit);

    // --- Top-p.
    m_topPEdit = new QDoubleSpinBox(this);
    m_topPEdit->setObjectName(QStringLiteral("topPEdit"));
    m_topPEdit->setRange(AppSettings::topPMin, AppSettings::topPMax);
    m_topPEdit->setDecimals(2);
    m_topPEdit->setSingleStep(0.05);
    m_topPEdit->setValue(values.topP);
    m_topPEdit->setMinimumWidth(110);
    form->addRow(tr("Top-p:"), m_topPEdit);

    // --- GPU-слои: -1 (минимум) показывается как «все слои».
    m_gpuLayersEdit = new QSpinBox(this);
    m_gpuLayersEdit->setObjectName(QStringLiteral("gpuLayersEdit"));
    m_gpuLayersEdit->setRange(AppSettings::gpuLayersMin,
                              AppSettings::gpuLayersMax);
    m_gpuLayersEdit->setSpecialValueText(tr("Все слои (авто)"));
    m_gpuLayersEdit->setToolTip(
        tr("-1 — все слои на GPU (Metal/CUDA); 0 — только CPU"));
    m_gpuLayersEdit->setValue(values.gpuLayers);
    // «Все слои (авто)» не помещается в sizeHint спинбокса.
    m_gpuLayersEdit->setMinimumWidth(160);
    form->addRow(tr("GPU-слои:"), m_gpuLayersEdit);

    // --- Интервал debounce.
    m_debounceEdit = new QSpinBox(this);
    m_debounceEdit->setObjectName(QStringLiteral("debounceEdit"));
    m_debounceEdit->setRange(AppSettings::debounceMin,
                             AppSettings::debounceMax);
    m_debounceEdit->setSingleStep(50);
    m_debounceEdit->setSuffix(QStringLiteral(" мс"));
    m_debounceEdit->setValue(values.debounceMs);
    m_debounceEdit->setMinimumWidth(110);
    form->addRow(tr("Пауза перед подсказкой:"), m_debounceEdit);

    // --- Автоматические подсказки.
    m_autoSuggestionsCheck = new QCheckBox(
        tr("Автоматические подсказки (по паузе в печати)"), this);
    m_autoSuggestionsCheck->setObjectName(
        QStringLiteral("autoSuggestionsCheck"));
    m_autoSuggestionsCheck->setChecked(values.autoSuggestions);
    form->addRow(QString(), m_autoSuggestionsCheck);

    // --- Стиль оформления (схема темы): default — системные цвета
    // шрифтов и контролов, ПТЮЧ — тёмная, светлый — светлая.
    // Применяется сразу в MainWindow::applySettings.
    m_styleCombo = new QComboBox(this);
    m_styleCombo->setObjectName(QStringLiteral("styleCombo"));
    for (const Theme::Scheme scheme : {Theme::Scheme::System,
                                       Theme::Scheme::Ptuch,
                                       Theme::Scheme::Light}) {
        m_styleCombo->addItem(Theme::schemeTitle(scheme),
                              Theme::schemeId(scheme));
    }
    m_styleCombo->setToolTip(
        tr("default — системные цвета шрифтов и контролов; "
           "ПТЮЧ — тёмная тема; светлый — светлая тема"));
    // Текущая схема из настроек (id санирован load()); стартовое
    // значение — до connect, сигналов не даём.
    const int schemeIndex = m_styleCombo->findData(values.style);
    m_styleCombo->setCurrentIndex(schemeIndex >= 0 ? schemeIndex : 0);
    m_styleCombo->setMinimumWidth(140);
    form->addRow(tr("Стиль:"), m_styleCombo);

    // --- Тест модели + метка результата (ошибка загрузки видна здесь).
    m_testButton = new QPushButton(tr("Test Model"), this);
    m_testButton->setObjectName(QStringLiteral("testModelButton"));
    connect(m_testButton, &QPushButton::clicked,
            this, &SettingsDialog::testModel);

    m_testStatusLabel = new QLabel(tr("Модель не проверялась"), this);
    m_testStatusLabel->setObjectName(QStringLiteral("testStatusLabel"));
    m_testStatusLabel->setWordWrap(true);

    auto* testRow = new QHBoxLayout;
    testRow->setContentsMargins(0, 0, 0, 0);
    testRow->addWidget(m_testButton);
    testRow->addWidget(m_testStatusLabel, 1);

    mainLayout->addLayout(form);
    mainLayout->addLayout(testRow);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("buttonBox"));
    connect(buttons, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

SettingsDialog::~SettingsDialog()
{
    // Если тест ещё идёт — обрываем и ждём поток, пока диалог (контекст
    // объектов) ещё жив; сигналы отключаются первыми, callbacks не сработают.
    stopTestBackend();
}

AppSettings SettingsDialog::settings() const
{
    AppSettings value;
    value.modelPath = m_modelPathEdit->text().trimmed();
    value.contextSize = m_contextSizeEdit->value();
    value.maxTokens = m_maxTokensEdit->value();
    value.temperature = m_temperatureEdit->value();
    value.topP = m_topPEdit->value();
    value.gpuLayers = m_gpuLayersEdit->value();
    value.debounceMs = m_debounceEdit->value();
    value.autoSuggestions = m_autoSuggestionsCheck->isChecked();
    value.style = m_styleCombo->currentData().toString();
    // Диапазоны виджетов уже гарантированы; повторная санитизация —
    // защита в глубину перед QSettings и LlamaBackend.
    value.sanitize();
    return value;
}

void SettingsDialog::browseModelPath()
{
    QString directory;
    const QString current = m_modelPathEdit->text();
    if (!current.isEmpty())
        directory = QFileInfo(current).absolutePath();

    const QString file = QFileDialog::getOpenFileName(
        this, tr("Выберите GGUF-модель"), directory,
        tr("Модели GGUF (*.gguf);;Все файлы (*)"));

    if (!file.isEmpty())
        m_modelPathEdit->setText(file);
}

void SettingsDialog::testModel()
{
    // Повторный тест: предыдущий поток останавливается (идемпотентно).
    stopTestBackend();

    const AppSettings values = settings();

    // Путь из поля; пустой — авто-поиск (env -> QSettings -> models/),
    // та же логика, что у приложения.
    QString path = values.modelPath;
    if (path.isEmpty())
        path = resolveModelPath();

    if (path.isEmpty()) {
        showTestResult(tr("Путь к модели не задан, модель не найдена"),
                       TestStatus::Error);
        return;
    }
    if (!QFileInfo::exists(path)) {
        showTestResult(tr("Файл не найден: %1").arg(path),
                       TestStatus::Error);
        return;
    }

    // Временный backend в собственном потоке: приложение продолжает
    // работать на СВОЁМ backend'е — активная генерация не отменяется
    // и модель не перезагружается.
    m_testThread = new QThread(this);
    m_testThread->setObjectName(QStringLiteral("llamaSettingsTest"));
    m_testBackend = new LlamaBackend;
    m_testBackend->moveToThread(m_testThread);
    connect(m_testThread, &QThread::finished,
            m_testBackend, &QObject::deleteLater);

    // configure до старта потока (как в MainWindow). Параметры — из
    // виджетов, т.е. ещё НЕ сохранённые: тестится то, что видит
    // пользователь.
    m_testBackend->configure(path, values.contextSize,
                             values.gpuLayers);

    connect(m_testBackend, &LlamaBackend::progressChanged,
            this, [this](float progress) {
                showTestResult(
                    tr("Загрузка модели: %1%")
                        .arg(static_cast<int>(progress * 100.0f)),
                    TestStatus::Neutral);
            });
    connect(m_testBackend, &LlamaBackend::modelLoaded,
            this,
            [this](const QString& loadedPath, int nCtx,
                   qint64 elapsedMs) {
                showTestResult(
                    tr("OK: %1 (n_ctx=%2, загрузка %3 мс)")
                        .arg(QFileInfo(loadedPath).fileName())
                        .arg(nCtx)
                        .arg(elapsedMs),
                    TestStatus::Success);
                // Позже текущего слота: не останавливаем backend, пока
                // его же сигнал ещё обрабатывается.
                QTimer::singleShot(0, this,
                                   [this]() { stopTestBackend(); });
            });
    connect(m_testBackend, &LlamaBackend::modelLoadFailed,
            this, [this](const QString& error) {
                showTestResult(
                    tr("Ошибка загрузки: %1").arg(error),
                    TestStatus::Error);
                QTimer::singleShot(0, this,
                                   [this]() { stopTestBackend(); });
            });

    setTesting(true);
    showTestResult(tr("Загрузка модели..."), TestStatus::Neutral);

    m_testThread->start();
    // Загрузка — queued-событие в worker: UI не блокируется.
    QMetaObject::invokeMethod(m_testBackend, &LlamaBackend::loadModel,
                              Qt::QueuedConnection);
}

void SettingsDialog::setTesting(bool testing)
{
    // Во время теста кнопка неактивна (защита от параллельных тестов);
    // кнопки OK/Cancel остаются — деструктор корректно остановит тест.
    m_testButton->setDisabled(testing);
    m_testButton->setText(testing ? tr("Тест...") : tr("Test Model"));
}

void SettingsDialog::stopTestBackend()
{
    if (m_testBackend != nullptr) {
        // Сигналы первыми: опоздавший modelLoaded/progressChanged не
        // перезапишет уже показанный результат.
        disconnect(m_testBackend, nullptr, this, nullptr);
        // Атомарно прерывает загрузку (progress_callback -> false) и
        // генерацию; llama-вызовов в UI-потоке здесь нет.
        m_testBackend->requestStop();
    }

    if (m_testThread != nullptr) {
        m_testThread->quit();
        if (!m_testThread->wait(10000)) {
            // Не должно случиться: requestStop обрывает загрузку на
            // ближайшем progress_callback. Страховка, чтобы не удалить
            // всё ещё работающий QThread.
            qWarning("SettingsDialog: тест-поток llama не остановился");
            m_testThread->wait();
        }
        // Объект backend освобождён по finished (deleteLater в потоке
        // объекта); сам QThread — потомок диалога, умрёт вместе с ним.
        m_testBackend = nullptr;
        m_testThread = nullptr;
    }

    setTesting(false);
}

void SettingsDialog::showTestResult(const QString& text,
                                    TestStatus status)
{
    // Цвет статуса — из цветов схемы (та же красная ошибка, что на
    // индикаторе состояния; нейтральный — вторичный текст темы).
    // Системная схема цветов не даёт (invalid) — метка остаётся
    // со системным шрифтом, различие — в тексте статуса.
    const Theme::Colors colors = Theme::colors(m_scheme);
    QColor color;
    if (status == TestStatus::Success)
        color = colors.success;
    else if (status == TestStatus::Error)
        color = colors.error;
    else
        color = colors.dimTextColor;

    m_testStatusLabel->setText(text);
    m_testStatusLabel->setStyleSheet(
        color.isValid()
            ? QStringLiteral("color: %1; font-weight: bold;")
                  .arg(color.name(QColor::HexRgb))
            : QString());
}
