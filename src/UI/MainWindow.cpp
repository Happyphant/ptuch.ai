#include "MainWindow.h"

#include "backend/mock_text_generation_backend.h"
#if PTUCH_DIAGNOSTICS
#include "diagnostics_dialog.h"
#endif
#include "document/text_file_io.h"
#include "llama/llama_backend.h"
#include "settings/app_settings.h"
#include "settings_dialog.h"
#include "suggestion/editor_adapter.h"
#include "suggestion/suggestion_controller.h"
#include "style_panel.h"
#include "suggestion_overlay.h"
#include "theme.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // Снимок системной палитры ДО первой темизации: схема default
    // возвращает к нему цвета шрифтов и контролов (см. applyTheme).
    m_systemPalette = qApp->palette();

    setWindowTitle(QStringLiteral("Ptuch Editor"));
    resize(1000, 700);

    createEditor();
    createStatusPanel();
    createFileMenu();
    createStylePanel();
    setupController();

    applyStyle();
    updateDocumentIndicator();
    updateStateIndicator(m_controller->state());
}

MainWindow::~MainWindow()
{
    // Сначала отвязываем контроллер (cancel + disconnect), затем
    // останавливаем llama-поток (прерывание загрузки/генерации) и
    // event loop mock-потока, и только потом ждём завершения —
    // объекты backend'ов освобождаются по finished своих потоков.
    if (m_controller != nullptr)
        m_controller->shutdown();

    stopLlamaWorker();

    if (m_generationThread != nullptr) {
        m_generationThread->quit();
        m_generationThread->wait(3000);
    }
}

void MainWindow::createEditor()
{
    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName("mainEditor");
    m_editor->setPlaceholderText(
        QStringLiteral("Начните печатать текст..."));
    m_editor->installEventFilter(this);

    setCentralWidget(m_editor);

    // Ghost-подсказка живёт поверх viewport'а редактора; что рисовать —
    // контроллер сообщает сигналами (связка — в setupController()).
    m_overlay = new SuggestionOverlay(m_editor);
}

void MainWindow::createStatusPanel()
{
    auto *bar = addToolBar(tr("Верхняя панель состояния"));
    bar->setObjectName("statusPanel");
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    m_stateIndicator = new QLabel(QStringLiteral("Ready"), bar);
    m_stateIndicator->setObjectName("stateIndicator");
    m_stateIndicator->setAlignment(Qt::AlignCenter);
    m_stateIndicator->setMinimumWidth(120);
    bar->addWidget(m_stateIndicator);

    bar->addSeparator();

    m_generateButton = new QPushButton(tr("Generate"), bar);
    m_generateButton->setObjectName("generateButton");
    bar->addWidget(m_generateButton);

    m_clearButton = new QPushButton(tr("Clear Suggestion"), bar);
    m_clearButton->setObjectName("clearButton");
    bar->addWidget(m_clearButton);

    m_settingsButton = new QPushButton(tr("Settings"), bar);
    m_settingsButton->setObjectName("settingsButton");
    bar->addWidget(m_settingsButton);

#if PTUCH_DIAGNOSTICS
    // Режим диагностики backend'а: снимок метрик последнего успешного
    // запроса (см. DiagnosticsDialog). В Release вырезается опцией
    // PTUCH_DIAGNOSTICS=OFF — кнопка, диалог и их тесты не компилируются.
    m_diagnosticsButton = new QPushButton(tr("Diagnostics"), bar);
    m_diagnosticsButton->setObjectName("diagnosticsButton");
    bar->addWidget(m_diagnosticsButton);
#endif

    bar->addSeparator();

    // Растягивающийся вставкой блок: line edit прижимается вправо.
    auto *spacer = new QWidget(bar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    m_textInput = new QLineEdit(bar);
    m_textInput->setObjectName("textInput");
    m_textInput->setPlaceholderText(QStringLiteral("Введите текст..."));
    m_textInput->setMinimumWidth(220);
    bar->addWidget(m_textInput);

    m_comboBox = new QComboBox(bar);
    m_comboBox->setObjectName("modeCombo");
    m_comboBox->addItems({QStringLiteral("Режим 1"),
                          QStringLiteral("Режим 2"),
                          QStringLiteral("Режим 3"),
                          QStringLiteral("Режим 4")});
    bar->addWidget(m_comboBox);

    // Имя файла + modified state (правый край; текст — updateDocumentIndicator).
    m_documentIndicator = new QLabel(bar);
    m_documentIndicator->setObjectName("documentIndicator");
    m_documentIndicator->setAlignment(Qt::AlignCenter);
    bar->addWidget(m_documentIndicator);
}

void MainWindow::createStylePanel()
{
    // Правый dock: панель сама держит модель (StyleMixer) и приносит
    // собственный тёмный stylesheet. MainWindow не считает микс —
    // только связка сигнала с контроллером (см. setupController()).
    auto *dock = new QDockWidget(tr("Стили"), this);
    dock->setObjectName(QStringLiteral("styleDock"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_stylePanel = new StylePanel(dock); // objectName — из конструктора
    dock->setWidget(m_stylePanel);

    addDockWidget(Qt::RightDockWidgetArea, dock);
}

void MainWindow::createFileMenu()
{
    // Команды документа в меню «Файл» (на macOS уходит в системное
    // меню). Горячие клавиши весят на QAction: Ctrl+N/O/S —
    // QKeySequence::New/Open/Save (на macOS — Cmd), Save As —
    // SaveAs (Ctrl+Shift+S). Контекст QAction — окно: работает,
    // пока оно активно.
    QMenu* fileMenu = menuBar()->addMenu(tr("Файл"));

    QAction* newAction = fileMenu->addAction(
        tr("Новый"), QKeySequence::New, this, &MainWindow::newDocument);
    newAction->setObjectName(QStringLiteral("actionNew"));

    QAction* openAction = fileMenu->addAction(
        tr("Открыть..."), QKeySequence::Open, this,
        &MainWindow::openDocumentDialog);
    openAction->setObjectName(QStringLiteral("actionOpen"));

    fileMenu->addSeparator();

    QAction* saveAction = fileMenu->addAction(
        tr("Сохранить"), QKeySequence::Save, this,
        &MainWindow::saveDocument);
    saveAction->setObjectName(QStringLiteral("actionSave"));

    QAction* saveAsAction = fileMenu->addAction(
        tr("Сохранить как..."), QKeySequence::SaveAs, this,
        &MainWindow::saveDocumentAsDialog);
    saveAsAction->setObjectName(QStringLiteral("actionSaveAs"));
}

// --- Документ v1: UTF-8 plain text -------------------------------------

bool MainWindow::maybeSave()
{
    if (m_editor == nullptr ||
        !m_editor->document()->isModified())
        return true;

    const QString name = m_filePath.isEmpty()
        ? tr("Без имени")
        : QFileInfo(m_filePath).fileName();

    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this,
        tr("Несохранённые изменения"),
        tr("Документ «%1» изменён. Сохранить изменения?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (choice == QMessageBox::Save)
        return saveDocument(); // ошибка записи/отмена Save As -> false
    if (choice == QMessageBox::Cancel)
        return false;
    return true; // Discard — продолжаем без сохранения
}

void MainWindow::newDocument()
{
    if (!maybeSave())
        return;

    setDocumentText(QString(), QString());
    statusBar()->showMessage(tr("Новый документ"));
}

bool MainWindow::openFile(const QString& path)
{
    // Подтверждение ДО чтения: при ошибке чтения текст не потерян.
    if (!maybeSave())
        return false;

    const TextLoadResult loaded = TextFileIO::loadUtf8(path);
    if (!loaded.ok) {
        QMessageBox::warning(
            this, tr("Ошибка открытия"),
            tr("Не удалось открыть файл «%1»:\n%2")
                .arg(path, loaded.error));
        return false;
    }

    setDocumentText(loaded.text, path);
    statusBar()->showMessage(tr("Открыто: %1").arg(path));
    return true;
}

bool MainWindow::openDocumentDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Открыть файл"),
        m_filePath.isEmpty() ? QString()
                             : QFileInfo(m_filePath).absolutePath(),
        tr("Текстовые файлы (*.txt);;Все файлы (*)"));

    if (path.isEmpty())
        return false; // отмена диалога
    return openFile(path);
}

bool MainWindow::saveDocument()
{
    if (m_filePath.isEmpty())
        return saveDocumentAsDialog(); // файла ещё нет — Save As
    return writeFile(m_filePath);
}

bool MainWindow::saveFileTo(const QString& path)
{
    // Ядро Save As: путь фиксируется только после УСПЕШНОЙ записи —
    // сбой записи оставляет прежнее имя файла.
    if (!writeFile(path))
        return false;

    m_filePath = path;
    updateDocumentIndicator();
    return true;
}

bool MainWindow::saveDocumentAsDialog()
{
    QFileDialog dialog(this, tr("Сохранить как"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(
        tr("Текстовые файлы (*.txt);;Все файлы (*)"));
    // Ввод имени без расширения дополняет .txt.
    dialog.setDefaultSuffix(QStringLiteral("txt"));
    dialog.setObjectName(QStringLiteral("saveFileDialog"));
    if (!m_filePath.isEmpty())
        dialog.selectFile(m_filePath);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    const QStringList files = dialog.selectedFiles();
    if (files.isEmpty())
        return false;
    return saveFileTo(files.first());
}

bool MainWindow::writeFile(const QString& path)
{
    const TextSaveResult saved =
        TextFileIO::saveUtf8(path, m_editor->toPlainText());
    if (!saved.ok) {
        QMessageBox::warning(
            this, tr("Ошибка сохранения"),
            tr("Не удалось сохранить файл «%1»:\n%2")
                .arg(path, saved.error));
        return false;
    }

    m_editor->document()->setModified(false);
    updateDocumentIndicator();
    statusBar()->showMessage(tr("Сохранено: %1").arg(path));
    return true;
}

void MainWindow::setDocumentText(const QString& text,
                                 const QString& filePath)
{
    // Пауза генерации на время подмены документа: контроллер выключен —
    // textChanged от setPlainText/clear не запланирует debounce, запрос
    // «в полёте» отменяется, показанная подсказка снимается («после
    // открытия очищать актуальную подсказку»). Обратное включение ничего
    // не запускает — генерация во время загрузки не стартует.
    if (m_controller != nullptr)
        m_controller->setEnabled(false);

    m_filePath = filePath;
    m_editor->setPlainText(text);
    m_editor->document()->setModified(false);

    if (m_controller != nullptr)
        m_controller->setEnabled(true);

    updateDocumentIndicator();
}

void MainWindow::updateDocumentIndicator()
{
    const QString name = m_filePath.isEmpty()
        ? tr("Без имени")
        : QFileInfo(m_filePath).fileName();
    const bool modified = m_editor->document()->isModified();

    // Имя + состояние в панели; в заголовке — «имя[*]»: маркер
    // изменённости рисует setWindowModified (звёздочка/точка закрытия).
    m_documentIndicator->setText(
        tr("%1 • %2").arg(name,
                          modified ? tr("изменён")
                                   : tr("не изменён")));
    setWindowTitle(tr("%1[*] — Ptuch Editor").arg(name));
    setWindowModified(modified);
}

void MainWindow::setupController()
{
    // Адаптер даёт контроллеру доступ к тексту и курсору редактора.
    // MainWindow только создаёт объекты и связывает сигналы —
    // логики inference здесь нет.
    m_editorAdapter = new PlainTextEditorAdapter(m_editor, this);

    // Mock-backend живёт в отдельном (не UI) потоке: генерация
    // «подсказки» не блокирует интерфейс. Родителя нет (иначе
    // moveToThread не сработает), освобождение — по finished потока.
    m_generationThread = new QThread(this);
    m_generationThread->setObjectName(QStringLiteral("generationThread"));
    m_generationBackend = new MockTextGenerationBackend;
    m_generationBackend->moveToThread(m_generationThread);

    connect(m_generationThread, &QThread::finished,
            m_generationBackend, &QObject::deleteLater);
    m_generationThread->start();

    m_controller = new SuggestionController(m_editorAdapter, this);
    // Единая DI-точка: подмена mock на реальный backend (llama) —
    // тот же setBackend() с другим объектом ITextGenerationBackend.
    m_controller->setBackend(m_generationBackend);

    // Настройки из QSettings — до первого запроса. Значения
    // санированы в AppSettings::load(), контроллер дополнительно
    // клампит debounce-интервал.
    const AppSettings appSettings = AppSettings::load();
    m_controller->setDebounceInterval(appSettings.debounceMs);
    m_controller->setAutoSuggestions(appSettings.autoSuggestions);
    m_controller->setGenerationParams(
        /*systemPrompt*/ QString(), appSettings.maxTokens,
        appSettings.temperature, appSettings.topP);

    // Редактор -> контроллер
    connect(m_editor, &QPlainTextEdit::textChanged,
            m_controller, &SuggestionController::onTextChanged);
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged,
            m_controller, &SuggestionController::onCursorPositionChanged);

    // Имя файла / modified state следят за изменениями документа
    // (типовые undo/redo тоже проходят через textChanged).
    connect(m_editor, &QPlainTextEdit::textChanged,
            this, &MainWindow::updateDocumentIndicator);

    // Кнопки -> контроллер
    connect(m_generateButton, &QPushButton::clicked,
            m_controller, &SuggestionController::requestSuggestion);
    connect(m_clearButton, &QPushButton::clicked,
            m_controller, &SuggestionController::rejectSuggestion);

    // Настройки -> диалог -> applySettings (см. openSettings).
    connect(m_settingsButton, &QPushButton::clicked,
            this, &MainWindow::openSettings);

#if PTUCH_DIAGNOSTICS
    // Диагностика -> диалог со снимком активного backend'а.
    connect(m_diagnosticsButton, &QPushButton::clicked,
            this, &MainWindow::openDiagnostics);
#endif

    // Стилевой микс -> контроллер: единый сигнал панели. Сам
    // setStyleMix() убирает показанную ghost-подсказку (она сделана
    // под прежний микс) и перепланирует запрос с новыми весами —
    // MainWindow здесь ничего не считает, только соединяет.
    connect(m_stylePanel, &StylePanel::styleMixChanged,
            m_controller, &SuggestionController::setStyleMix);

    // Профили (нейтральные QString/double-поля) — контроллер проверяет
    // adapterPath активных стилей перед отправкой запроса (явные ошибки
    // вместо тихого игнора; см. README «Стили и LoRA-адаптеры»). Ни в
    // профилях, ни в панели нет типов библиотек обучения. Профили в MVP
    // статичны — отдаём один раз; изменения весов идут через
    // styleMixChanged выше.
    m_controller->setStyleProfiles(m_stylePanel->mixer().profiles());

    // Контроллер -> индикатор состояния (сам контроллер виджетов не знает)
    connect(m_controller, &SuggestionController::stateChanged,
            this, &MainWindow::updateStateIndicator);

    // Контроллер -> ghost overlay: показать/скрыть подсказку.
    // Overlay сам следит за курсором, текстом и прокруткой при отрисовке.
    connect(m_controller, &SuggestionController::suggestionReady,
            m_overlay, &SuggestionOverlay::setSuggestion);
    connect(m_controller, &SuggestionController::suggestionCleared,
            m_overlay, &SuggestionOverlay::clear);

    // Контроллер -> статусная строка
    connect(m_controller, &SuggestionController::suggestionStarted,
            this, [this]() {
                statusBar()->showMessage(tr("Генерация подсказки..."));
            });
    connect(m_controller, &SuggestionController::suggestionReady,
            this, [this](const QString &text, quint64) {
                statusBar()->showMessage(
                    tr("Подсказка готова: %1").arg(text));
            });
    connect(m_controller, &SuggestionController::suggestionFailed,
            this, [this](const QString &error, quint64) {
                statusBar()->showMessage(error);
            });
    connect(m_controller, &SuggestionController::suggestionCleared,
            this, [this]() {
                statusBar()->showMessage(tr("Подсказка очищена"));
            });

    // GGUF-модель подключается после контроллера: при успешной загрузке
    // подменяет mock через ту же DI-точку setBackend().
    setupLlamaBackend();
}

void MainWindow::setupLlamaBackend()
{
    // Отключение окружением (UI-тесты не должны грузить реальный GGUF).
    if (qEnvironmentVariableIsSet("PTUCH_DISABLE_LLAMA")) {
        statusBar()->showMessage(
            tr("llama.cpp отключён (PTUCH_DISABLE_LLAMA) — mock-бэкенд"));
        return;
    }

    const QString modelPath = resolveModelPath();
    if (modelPath.isEmpty()) {
        statusBar()->showMessage(
            tr("GGUF-модель не найдена — подсказки от mock-бэкенда"));
        return;
    }

    // Тот же паттерн, что у mock: объект без родителя, свой поток,
    // освобождение по finished. Вся llama-активность — в m_llamaThread.
    m_llamaThread = new QThread(this);
    m_llamaThread->setObjectName(QStringLiteral("llamaWorker"));
    m_llamaBackend = new LlamaBackend;
    m_llamaBackend->moveToThread(m_llamaThread);

    connect(m_llamaThread, &QThread::finished,
            m_llamaBackend, &QObject::deleteLater);

    // Конфигурация ДО старта потока: путь из настроек/поиска; n_ctx и
    // GPU-слои — из AppSettings (санированы при загрузке).
    const AppSettings appSettings = AppSettings::load();
    m_llamaBackend->configure(modelPath, appSettings.contextSize,
                              appSettings.gpuLayers);

    // Связи — до старта потока, чтобы не пропустить сигналы загрузки.
    connect(m_llamaBackend, &LlamaBackend::progressChanged,
            this, [this](float progress) {
                statusBar()->showMessage(
                    tr("Загрузка модели: %1%")
                        .arg(static_cast<int>(progress * 100.0f)));
            });
    connect(m_llamaBackend, &LlamaBackend::modelLoaded,
            this, [this](const QString &path, int contextSize,
                         qint64 elapsedMs) {
                statusBar()->showMessage(
                    tr("Модель загружена: %1 (n_ctx=%2, %3 мс)")
                        .arg(QFileInfo(path).fileName())
                        .arg(contextSize)
                        .arg(elapsedMs));
                // Единая DI-точка: подменяем mock на llama. Контроллер
                // сам отменит активный запрос и переподключит сигналы.
                m_controller->setBackend(m_llamaBackend);
            });
    connect(m_llamaBackend, &LlamaBackend::modelLoadFailed,
            this, [this](const QString &error) {
                // Остаёмся на mock: подсказки продолжают работать.
                statusBar()->showMessage(
                    tr("llama.cpp: %1 — остаёмся на mock").arg(error));
            });

    m_llamaThread->start();

    // Загрузка GGUF — queued-событие в worker: UI не блокируется
    // (никаких sleep/ожиданий в UI-потоке).
    QMetaObject::invokeMethod(m_llamaBackend, &LlamaBackend::loadModel,
                              Qt::QueuedConnection);
}

void MainWindow::stopLlamaWorker()
{
    if (m_llamaBackend != nullptr) {
        // Сигналы первыми: опоздавшие modelLoaded/progress не тронут UI.
        disconnect(m_llamaBackend, nullptr, this, nullptr);
        // Атомарно прерывает загрузку (progress_callback -> false) и
        // текущую генерацию; вызов безопасен из UI-потока — llama-
        // вызовов здесь нет.
        m_llamaBackend->requestStop();
    }

    if (m_llamaThread != nullptr) {
        m_llamaThread->quit();
        m_llamaThread->wait(5000);
        // После wait() объект освобождён (deleteLater по finished) —
        // обнуляем, чтобы повторный вызов (деструктор) был безопасным.
        m_llamaBackend = nullptr;
    }
}

void MainWindow::openSettings()
{
    // exec() пускает вложенный event loop: диалог модален, приложение
    // при этом продолжает работать (debounce/mock-поток не висят).
    SettingsDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    applySettings(dialog.settings());
}

#if PTUCH_DIAGNOSTICS
void MainWindow::openDiagnostics()
{
    // Только чтение готового снимка через интерфейс backend'а — логики
    // inference здесь нет; текст диалога формируется из полей снимка
    // (без указателей и внутренних адресов backend'а).
    const ITextGenerationBackend* backend =
        m_controller != nullptr ? m_controller->backend() : nullptr;
    const BackendDiagnostics data =
        backend != nullptr ? backend->diagnostics() : BackendDiagnostics();

    DiagnosticsDialog dialog(data, this);
    dialog.exec();
}
#endif

void MainWindow::applySettings(const AppSettings& settings)
{
    const AppSettings previous = AppSettings::load();
    // save() санирует перед записью; переживает перезапуск (sync).
    settings.save();

    // 1) Настройки, не влияющие на модель, — сразу и без отмен:
    //    контроллер копирует параметры в КАЖДЫЙ новый запрос, поэтому
    //    активная генерация продолжает идти со старыми значениями и
    //    завершается штатно; debounce-таймер «в полёте» не сбрасывается.
    if (m_controller != nullptr) {
        m_controller->setDebounceInterval(settings.debounceMs);
        m_controller->setAutoSuggestions(settings.autoSuggestions);
        m_controller->setGenerationParams(
            /*systemPrompt*/ QString(), settings.maxTokens,
            settings.temperature, settings.topP);
    }

    // 2) Путь/n_ctx/GPU-слои фиксируются при создании llama_context:
    //    перезагрузка нужна ТОЛЬКО при их реальном изменении. Если
    //    менялись только параметры генерации/debounce — активная
    //    генерация не затрагивается вовсе.
    const bool modelAffectingChanged =
        previous.modelPath != settings.modelPath ||
        previous.contextSize != settings.contextSize ||
        previous.gpuLayers != settings.gpuLayers;

    if (modelAffectingChanged)
        reloadLlamaBackend();

    // 3) Тема: схема применяется сразу — палитра приложения и
    //    stylesheet'ы окна/панели (для default — возврат к системной
    //    палитре и снятие stylesheet'ов, см. src/UI/theme.h).
    if (previous.style != settings.style)
        applyTheme(Theme::schemeFromId(settings.style));
}

void MainWindow::reloadLlamaBackend()
{
    // llama отключён окружением — перезагружать нечего (mock работает).
    if (qEnvironmentVariableIsSet("PTUCH_DISABLE_LLAMA"))
        return;

    // Безопасный свап: никогда не освобождаем llama_context «под»
    // decode. Порядок:
    //  1) контроллер первым переходит на mock — cancel + bump id
    //     корректно завершают активный запрос как устаревший, сигналы
    //     старого backend'а отвязаны (опоздавший modelLoaded не
    //     переподключится);
    //  2) старый поток гаснет: requestStop атомарно прерывает
    //     загрузку/декод, wait() дожидается выхода из llama-циклов ДО
    //     освобождения объекта (RAII в потоке) — use-after-free
    //     исключён;
    //  3) новый backend стартует со свежими настройками. Пока модель
    //     грузится, подсказки отдаёт mock — UI не блокируется.
    if (m_controller != nullptr && m_generationBackend != nullptr)
        m_controller->setBackend(m_generationBackend);

    stopLlamaWorker();
    setupLlamaBackend();
}

void MainWindow::updateStateIndicator(SuggestionController::State state)
{
    // Заголовок состояния — всегда; цвет — из темы (error красный,
    // generating электрик-синий и т.д.), инлайновый stylesheet метки,
    // фон/рамка остаются от QLabel#stateIndicator. Системная схема
    // цвет не задаёт (invalid) — надпись остаётся системной.
    m_stateIndicator->setText(Theme::stateTitle(state));
    const QColor color = Theme::stateColor(m_scheme, state);
    m_stateIndicator->setStyleSheet(
        color.isValid()
            ? QStringLiteral("color: %1; font-weight: bold;")
                  .arg(color.name(QColor::HexRgb))
            : QString());
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_editor && m_controller != nullptr) {
        // Потеря фокуса редактором (клик по панели/диалог/переключение
        // окна): подсказка скрывается, debounce и активный запрос
        // гасятся — контроллер сам, eventFilter только сообщает.
        if (event->type() == QEvent::FocusOut) {
            m_controller->onFocusLost();
            return QMainWindow::eventFilter(watched, event);
        }

        // Клавиши переводятся здесь: контроллер не знает про QKeyEvent/виджеты.
        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);

            // Ctrl+Space — немедленный запрос: минует debounce и
            // стартует генерацию синхронно (тот же путь, что Generate).
            if (keyEvent->key() == Qt::Key_Space &&
                keyEvent->modifiers() == Qt::ControlModifier) {
                m_controller->requestSuggestion();
                return true;
            }

            // Tab — принять показанную подсказку (вставка в документ).
            // hasSuggestion() — только актуальная: старая подсказка
            // очищена ещё при вводе/смене курсора/потере фокуса.
            if (keyEvent->key() == Qt::Key_Tab &&
                keyEvent->modifiers() == Qt::NoModifier &&
                m_controller->hasSuggestion()) {
                const QString accepted = m_controller->suggestion();
                m_controller->acceptSuggestion();
                statusBar()->showMessage(
                    tr("Подсказка принята: %1").arg(accepted));
                return true;
            }

            // Shift+Tab — запросить альтернативную подсказку.
            if (keyEvent->key() == Qt::Key_Tab &&
                keyEvent->modifiers() == Qt::ShiftModifier &&
                m_controller->hasSuggestion()) {
                m_controller->requestAlternative();
                return true;
            }

            // Escape — отклонить подсказку, debounce или запрос в полёте.
            if (keyEvent->key() == Qt::Key_Escape &&
                (m_controller->hasSuggestion() ||
                 m_controller->state() ==
                     SuggestionController::State::Debouncing ||
                 m_controller->state() ==
                     SuggestionController::State::Generating)) {
                m_controller->rejectSuggestion();
                return true;
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Несохранённые изменения — ПЕРЕД shutdown:
    //  Cancel  -> ignore: окно живо, контроллер/потоки НЕ остановлены
    //            (иначе после отмены подсказки бы отвалились);
    //  Save    -> запись; сбой/отмена Save As тоже остаёмся в окне;
    //  Discard -> закрываемся штатно.
    if (!maybeSave()) {
        event->ignore();
        return;
    }

    // Останавливаем debounce, отменяем активный запрос и отвязываем
    // backend до разрушения виджетов: таймер и колбэки больше не
    // сработают (опоздавшие отсекаются QPointer'ом в контроллере).
    if (m_controller != nullptr)
        m_controller->shutdown();

    // Прерываем загрузку/генерацию llama и гасим его поток до
    // разрушения UI (идемпотентно — деструктор вызовет повторно).
    stopLlamaWorker();

    QMainWindow::closeEvent(event);
}

void MainWindow::applyStyle()
{
    // Схема темы (default / ПТЮЧ / светлый) — из настроек; применяется
    // и при старте, и при изменении в applySettings.
    applyTheme(Theme::schemeFromId(AppSettings::load().style));
}

void MainWindow::applyTheme(Theme::Scheme scheme)
{
    m_scheme = scheme;

    if (scheme == Theme::Scheme::System) {
        // Системная схема: палитра — к снимку до темизации, все
        // stylesheet'ы снимаются — цвета шрифтов и контролов
        // платформенные (включая зону набора).
        qApp->setPalette(m_systemPalette);
        setStyleSheet(QString());
    } else {
        // Палитра приложения — базовые роли для всех виджетов (в т.ч.
        // тех, что идут без stylesheet) и для ghost-подсказки (она
        // красит по Base/Text viewport'а — эти цвета совпадают с фоном
        // редактора и его текстом).
        qApp->setPalette(Theme::palette(scheme));
        // Stylesheet окна: состояния контролов (hover/pressed/
        // disabled/focus), состояния контроллера на индикаторе, зона
        // набора без визуального шума.
        setStyleSheet(Theme::mainStyleSheet(scheme));
    }

    // Панель стилей живёт со своим stylesheet — та же схема.
    if (m_stylePanel != nullptr)
        m_stylePanel->setScheme(scheme);

    // Индикатор состояния: инлайновый цвет перерисовывается под новую
    // схему (System снимает его совсем — надпись остаётся).
    if (m_controller != nullptr && m_stateIndicator != nullptr)
        updateStateIndicator(m_controller->state());
}
