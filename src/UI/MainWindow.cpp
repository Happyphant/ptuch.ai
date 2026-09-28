#include "MainWindow.h"

#include "backend/mock_text_generation_backend.h"
#include "llama/llama_backend.h"
#include "suggestion/editor_adapter.h"
#include "suggestion/suggestion_controller.h"
#include "suggestion_overlay.h"

#include <QComboBox>
#include <QCloseEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Ptuch Editor"));
    resize(1000, 700);

    createEditor();
    createStatusPanel();
    setupController();

    applyStyle();
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

    // Редактор -> контроллер
    connect(m_editor, &QPlainTextEdit::textChanged,
            m_controller, &SuggestionController::onTextChanged);
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged,
            m_controller, &SuggestionController::onCursorPositionChanged);

    // Кнопки -> контроллер
    connect(m_generateButton, &QPushButton::clicked,
            m_controller, &SuggestionController::requestSuggestion);
    connect(m_clearButton, &QPushButton::clicked,
            m_controller, &SuggestionController::rejectSuggestion);

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

    // Конфигурация ДО queued-вызова loadModel: путь из настроек/поиска,
    // контекст ограничен, все слои на GPU (Metal).
    m_llamaBackend->configure(modelPath, /*contextSize*/ 4096,
                              /*gpuLayers*/ -1);

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

void MainWindow::updateStateIndicator(SuggestionController::State state)
{
    QString title;
    QString color;

    switch (state) {
    case SuggestionController::State::Idle:
        title = QStringLiteral("Ready");
        color = QStringLiteral("#4caf50");
        break;
    case SuggestionController::State::Debouncing:
        title = QStringLiteral("Waiting");
        color = QStringLiteral("#ffb300");
        break;
    case SuggestionController::State::Generating:
        title = QStringLiteral("Generating");
        color = QStringLiteral("#42a5f5");
        break;
    case SuggestionController::State::Ready:
        title = QStringLiteral("Ready");
        color = QStringLiteral("#4caf50");
        break;
    case SuggestionController::State::Error:
        title = QStringLiteral("Error");
        color = QStringLiteral("#f44336");
        break;
    }

    m_stateIndicator->setText(title);
    m_stateIndicator->setStyleSheet(
        QStringLiteral("color: %1; font-weight: bold;").arg(color));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Клавиши переводятся здесь: контроллер не знает про QKeyEvent/виджеты.
    if (watched == m_editor &&
        event->type() == QEvent::KeyPress &&
        m_controller != nullptr) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);

        // Tab — принять показанную подсказку (вставка в документ).
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

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
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
    setStyleSheet(R"(
        QMainWindow {
            background-color: #3a3a3a;
        }

        QWidget {
            color: #f0f0f0;
            font-size: 13px;
        }

        QToolBar#statusPanel {
            background-color: #3a3a3a;
            border: none;
            border-bottom: 1px solid #2a2a2a;
            spacing: 8px;
            padding: 4px;
        }

        QLabel#stateIndicator {
            background-color: #454545;
            border: 1px solid #565656;
            border-radius: 4px;
            padding: 5px 10px;
        }

        QPushButton#generateButton, QPushButton#clearButton {
            background-color: #454545;
            border: 1px solid #565656;
            border-radius: 4px;
            padding: 6px 12px;
            color: #ffffff;
        }

        QPushButton#generateButton:hover,
        QPushButton#clearButton:hover {
            background-color: #505050;
        }

        QPushButton#generateButton:pressed,
        QPushButton#clearButton:pressed {
            background-color: #333333;
        }

        QLineEdit#textInput {
            background-color: #454545;
            border: 1px solid #565656;
            border-radius: 4px;
            padding: 6px 8px;
            color: #ffffff;
            selection-background-color: #6a6a6a;
        }

        QComboBox#modeCombo {
            background-color: #454545;
            border: 1px solid #565656;
            border-radius: 4px;
            padding: 4px 8px;
            color: #ffffff;
        }

        QComboBox#modeCombo::drop-down {
            border: none;
        }

        QComboBox#modeCombo QAbstractItemView {
            background-color: #454545;
            color: #ffffff;
            selection-background-color: #6a6a6a;
        }

        QLabel {
            color: #dddddd;
        }

        QStatusBar {
            background-color: #2f2f2f;
            color: #cccccc;
            border-top: 1px solid #2a2a2a;
        }

        QPlainTextEdit#mainEditor {
            background-color: #333333;
            color: #ffffff;
            border: none;
            padding: 8px;
            selection-background-color: #6a6a6a;
        }
    )");
}
