// settings_dialog.h
#pragma once

#include "settings/app_settings.h"
#include "theme.h" // Theme::Scheme (схема темы диалога)

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QThread;
class LlamaBackend;

// Диалог настроек (QSettings): путь к GGUF-модели, размер контекста,
// максимум новых токенов, temperature, top-p, GPU-слои, интервал
// debounce, включение автоматических подсказок и выбор стиля
// (default / ПТЮЧ / светлый; см. src/UI/theme.h).
//
// Валидация: числовые поля — QSpinBox/QDoubleSpinBox с диапазонами из
// AppSettings (полем выйти за границы нельзя), settings() санируется
// повторно; AppSettings::load() санирует ещё и правленый руками файл
// настроек — некорректное значение не доедет ни до контроллера, ни до
// LlamaBackend.
//
// «Тест модели» грузит GGUF на ВРЕМЕННОМ LlamaBackend в собственном
// worker-потоке (configure + queued loadModel): активная генерация
// приложения при этом не затрагивается — ни отменой, ни перезагрузкой
// модели. Результат (прогресс / успех / ошибка загрузки) показывается
// меткой рядом с кнопкой. Тест-поток останавливается
// disconnect -> requestStop -> quit -> wait — после результата, при
// повторном тесте и в деструкторе (идемпотентно).
class SettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override;

    // Значения из виджетов: в диапазонах AppSettings + sanitize().
    AppSettings settings() const;

private slots:
    void browseModelPath();
    void testModel();

private:
    // Статус результата Test Model (цвет метки).
    enum class TestStatus { Neutral, Success, Error };

    void setTesting(bool testing);
    // Остановка временного тест-потока: disconnect -> requestStop ->
    // quit -> wait -> обнуление. Идемпотентно.
    void stopTestBackend();
    void showTestResult(const QString& text, TestStatus status);

    QLineEdit* m_modelPathEdit = nullptr;
    QPushButton* m_browseButton = nullptr;
    QSpinBox* m_contextSizeEdit = nullptr;
    QSpinBox* m_maxTokensEdit = nullptr;
    QDoubleSpinBox* m_temperatureEdit = nullptr;
    QDoubleSpinBox* m_topPEdit = nullptr;
    QSpinBox* m_gpuLayersEdit = nullptr;
    QSpinBox* m_debounceEdit = nullptr;
    QCheckBox* m_autoSuggestionsCheck = nullptr;
    QComboBox* m_styleCombo = nullptr;
    // Схема темы, с которой открыт диалог (цвета статуса Test Model).
    Theme::Scheme m_scheme = Theme::Scheme::Ptuch;
    QPushButton* m_testButton = nullptr;
    QLabel* m_testStatusLabel = nullptr;

    // Временный backend для Test Model: объект без родителя (иначе
    // moveToThread не сработает), свой поток, освобождение по finished.
    QThread* m_testThread = nullptr;
    LlamaBackend* m_testBackend = nullptr;
};
