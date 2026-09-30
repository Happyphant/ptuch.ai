#pragma once

#include "suggestion/suggestion_controller.h"
#include "theme.h" // Theme::Scheme (схема темы окна)

#include <QMainWindow>
#include <QPalette>

class QComboBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QMenu;
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

    // Документ v1 (UTF-8 plain text): ядро команд БЕЗ диалогов —
    // путь подаёт вызывающий (диалог выбора файла или тест).
    // false — отмена подтверждения несохранённых изменений или ошибка
    // чтения/записи (диалог ошибки показывает сам метод).
    bool openFile(const QString& path);
    bool saveFileTo(const QString& path);

protected:
    // Перехват событий редактора: Tab/Shift+Tab/Escape/Ctrl+Space и
    // потеря фокуса — MainWindow переводит их в вызовы контроллера
    // (сам контроллер виджетов не видит).
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Отвязываем контроллер от backend до разрушения UI.
    void closeEvent(QCloseEvent *event) override;

private:
    void createEditor();
    void createStatusPanel();
    // Меню «Файл»: New/Open/Save/Save As + горячие клавиши
    // (QKeySequence::New/Open/Save/SaveAs — Ctrl+N/O/S, Ctrl+Shift+S).
    void createFileMenu();
    // Панель стилей (StylePanel) в правом dock: модель StyleMixer живёт
    // внутри панели, MainWindow только подключает сигнал к контроллеру.
    void createStylePanel();
    void setupController();
    // --- Документ v1: UTF-8 plain text ---------------------------------
    // Подтверждение несохранённых изменений (New/Open/close):
    // Save — записать (false, если запись не удалась/отменена),
    // Discard — продолжить без сохранения, Cancel — false (всё как было).
    bool maybeSave();
    // Новый пустой документ (с подтверждением).
    void newDocument();
    // Диалоги выбора пути (вызывают openFile/saveFileTo).
    bool openDocumentDialog();
    bool saveDocumentAsDialog();
    // Ctrl+S: запись в m_filePath либо Save As, если файла ещё нет.
    bool saveDocument();
    // Замена содержимого редактора (общее для New/Open): генерация на
    // паузе, modified сбрасывается, индикатор обновляется.
    void setDocumentText(const QString& text, const QString& filePath);
    // Запись файла + диалог ошибки + сброс modified/индикатора.
    bool writeFile(const QString& path);
    // Имя файла и modified state: индикатор в панели + заголовок окна
    // («имя[*]», маркер гасит setWindowModified).
    void updateDocumentIndicator();
    // -------------------------------------------------------------------
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
    // Индикатор в верхней панели по состоянию контроллера
    // (заголовок/цвет — из Theme::stateTitle/stateColor; для
    // системной схемы цвет не задаётся).
    void updateStateIndicator(SuggestionController::State state);
    // Тема приложения (см. src/UI/theme.h): читает ui/style из
    // настроек (default / ПТЮЧ / светлый) и зовёт applyTheme.
    void applyStyle();
    // Применение схемы: Theme::Scheme::System — палитра возвращается
    // к системному снимку m_systemPalette, stylesheet'ы снимаются
    // (шрифты и контролы платформенные); Ptuch/Light — палитра
    // приложения, stylesheet окна и StylePanel. Вызывается при старте
    // и при изменении ui/style в applySettings.
    void applyTheme(Theme::Scheme scheme);

    QPlainTextEdit *m_editor = nullptr;
    // Полупрозрачная ghost-подсказка поверх редактора (документ не трогает).
    SuggestionOverlay *m_overlay = nullptr;
    QLabel *m_stateIndicator = nullptr;
    // Имя текущего файла + modified state (правый край панели).
    QLabel *m_documentIndicator = nullptr;
    // Текущий файл; пусто = «Без имени» (Save уходит в Save As).
    QString m_filePath;
    QPushButton *m_generateButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    // Стилевой микшер: отдельный dock справа (создаётся всегда).
    StylePanel *m_stylePanel = nullptr;
    // Активная схема темы (для updateStateIndicator).
    Theme::Scheme m_scheme = Theme::Scheme::Ptuch;
    // Системная палитра, снятая ДО первой темизации (в приложении
    // MainWindow создаётся один раз) — Scheme::System возвращает её.
    QPalette m_systemPalette;
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
