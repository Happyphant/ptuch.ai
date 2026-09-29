#pragma once

#include "suggestion/suggestion_controller.h"

#include <QMainWindow>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QThread;

class MockTextGenerationBackend;
class LlamaBackend;
class PlainTextEditorAdapter;
class StylePanel;
class SuggestionOverlay;
struct AppSettings;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    // Перехват Tab/Shift+Tab/Escape: MainWindow переводит клавиши в
    // вызовы контроллера (сам контроллер виджетов не видит).
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Отвязываем контроллер от backend до разрушения UI.
    void closeEvent(QCloseEvent *event) override;

private:
    void createEditor();
    void createStatusPanel();
    // Панель стилей (StylePanel) в правом dock: модель StyleMixer живёт
    // внутри панели, MainWindow только подключает сигнал к контроллеру.
    void createStylePanel();
    void setupController();
    // GGUF: llama-бэкенд в отдельном worker-потоке; при отсутствии
    // модели остаётся mock (fallback через setBackend).
    void setupLlamaBackend();
    // requestStop (атомарно, из любого потока) + quit/wait llama-потока.
    // Идемпотентно: вызывается из closeEvent и деструктора.
    void stopLlamaWorker();
    // Диалог настроек (SettingsDialog) -> применение результата.
    void openSettings();
    // Режим диагностики backend'а: снимок BackendDiagnostics активного
    // backend'а -> DiagnosticsDialog. Определяется только при сборке
    // с PTUCH_DIAGNOSTICS=1 (см. CMake); кнопка в панели — тоже.
    void openDiagnostics();
    // Применение настроек: параметры генерации — сразу без отмен
    // (активная генерация продолжается); модельные (путь/n_ctx/GPU) —
    // только при реальном изменении, безопасным свапом.
    void applySettings(const AppSettings& settings);
    // Смена модельных настроек: контроллер -> mock (cancel + bump id),
    // остановка старого llama-потока, запуск нового с новыми параметрами.
    void reloadLlamaBackend();
    // Индикатор в верхней панели по состоянию контроллера.
    void updateStateIndicator(SuggestionController::State state);
    void applyStyle();

    QPlainTextEdit *m_editor = nullptr;
    // Полупрозрачная ghost-подсказка поверх редактора (документ не трогает).
    SuggestionOverlay *m_overlay = nullptr;
    QLabel *m_stateIndicator = nullptr;
    QPushButton *m_generateButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    // Стилевой микшер: отдельный dock справа (создаётся всегда).
    StylePanel *m_stylePanel = nullptr;
    // Режим диагностики: создаётся только при PTUCH_DIAGNOSTICS=1.
    QPushButton *m_diagnosticsButton = nullptr;
    QLineEdit *m_textInput = nullptr;
    QComboBox *m_comboBox = nullptr;

    // Подсистема подсказок (MainWindow только создаёт и связывает).
    PlainTextEditorAdapter *m_editorAdapter = nullptr;
    // Backend живёт в отдельном (не UI) потоке: генерация не блокирует UI.
    QThread *m_generationThread = nullptr;
    MockTextGenerationBackend *m_generationBackend = nullptr;
    // llama.cpp: модель загружается один раз, inference — только в
    // m_llamaThread (UI никогда не вызывает llama_decode/sampling).
    QThread *m_llamaThread = nullptr;
    LlamaBackend *m_llamaBackend = nullptr;
    SuggestionController *m_controller = nullptr;
};
