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
class SuggestionOverlay;

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
    void setupController();
    // GGUF: llama-бэкенд в отдельном worker-потоке; при отсутствии
    // модели остаётся mock (fallback через setBackend).
    void setupLlamaBackend();
    // requestStop (атомарно, из любого потока) + quit/wait llama-потока.
    // Идемпотентно: вызывается из closeEvent и деструктора.
    void stopLlamaWorker();
    // Индикатор в верхней панели по состоянию контроллера.
    void updateStateIndicator(SuggestionController::State state);
    void applyStyle();

    QPlainTextEdit *m_editor = nullptr;
    // Полупрозрачная ghost-подсказка поверх редактора (документ не трогает).
    SuggestionOverlay *m_overlay = nullptr;
    QLabel *m_stateIndicator = nullptr;
    QPushButton *m_generateButton = nullptr;
    QPushButton *m_clearButton = nullptr;
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
