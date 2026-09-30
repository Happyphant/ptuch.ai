// style_panel.h
#pragma once

#include "suggestion/style_mixer.h"
#include "theme.h" // Theme::Scheme (схема темы панели)

#include <QHash>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QSlider;

// Панель стилевого микша. Строка на каждый StyleProfile: цветовой
// маркер, название, слайдер 0..100, числовое значение слайдера,
// нормализованная доля, чекбокс включения и кнопка сброса; внизу —
// сумма весов активных профилей и кнопка Reset Mix.
//
// Модель отделена от UI: панель держит StyleMixer (доменный объект без
// виджетов) и только отображает его состояние. Путь изменения — виджет
// -> модель (setWeight/setEnabled) -> подписи -> сигнал; нормализацию
// и активность считает модель (activeTotal()/styleWeights()), не
// виджеты.
//
// Нейтральность относительно адаптеров (LoRA): панель отображает
// ТОЛЬКО нейтральные поля профиля (имя, описание, цвет, вес,
// включение) — поля adapterPath/adapterType/baseModelId/promptTag/
// adapterScale она не рисует и не интерпретирует, никаких типов
// библиотек обучения в панели нет (см. style_profile.h и README
// «Стили и LoRA-адаптеры»: MVP микшует стили через prompt).
//
// Единый сигнал styleMixChanged: нормализованные веса активных
// профилей (сумма 1.0) — готовы к SuggestionController::setStyleMix.
// Испускается ровно один раз на одно действие пользователя (слайдер,
// чекбокс, сброс профиля, Reset Mix); при отсутствии изменений
// сигнала нет.
//
// Циклические обновления исключены конструктивно:
//  - обработчики обновляют ТОЛЬКО подписи (QLabel) и никогда не пишут
//    обратно в слайдер/чекбокс — петля невозможна;
//  - программные setValue() при массовом Reset Mix — под
//    QSignalBlocker: один жест даёт ровно один сигнал;
//  - стартовые значения выставляются ДО подключения сигналов —
//    построение панели не «изменение микса».
//
// Тема: панель приносит собственный stylesheet из модуля темы
// (Theme::panelStyleSheet — фон/текст/акценты выбранной схемы)
// — корректно выглядит и внутри MainWindow, и отдельно от него.
// Схема (default / ПТЮЧ / светлый) переключается setScheme —
// её зовёт MainWindow из ui/style (default снимает stylesheet
// совсем — контролы панели становятся системными).
class StylePanel final : public QWidget
{
    Q_OBJECT

public:
    explicit StylePanel(QWidget* parent = nullptr);
    // DI: свой микшер (например, восстановленный из настроек).
    explicit StylePanel(StyleMixer mixer, QWidget* parent = nullptr);

    // Схема темы панели: пересборка stylesheet'а панели и цветовых
    // маркеров профилей (default — пустые, системный вид).
    void setScheme(Theme::Scheme scheme);

    // Модель данных (не виджет): для проверок и будущего сохранения.
    const StyleMixer& mixer() const { return m_mixer; }
    // Что уйдёт в контроллер: нормализованные веса активных профилей.
    QVector<StyleWeight> mix() const { return m_mixer.styleWeights(); }

signals:
    // Единый сигнал всех изменений микса (нормализованные веса).
    void styleMixChanged(const QVector<StyleWeight>& weights);

private:
    // Указатели на виджеты строки профиля (модель — в m_mixer).
    struct Row {
        QSlider* slider = nullptr;
        QLabel* valueLabel = nullptr;
        QLabel* normalizedLabel = nullptr;
        QCheckBox* enabledCheck = nullptr;
    };

    void buildUi();
    // Пересчёт подписей из модели: сумма, значения слайдеров,
    // нормализованные доли. Обновляет только QLabel — сигналов UI
    // не порождает (нет циклов).
    void refreshLabels();
    // Общая точка изменений: модель уже обновлена обработчиком —
    // освежить подписи и испустить ровно один styleMixChanged.
    void notifyMixChanged();
    // Применение темы: stylesheet панели + маркеры профилей
    // (цвет профиля + рамка/радиус схемы) по текущему m_scheme.
    void applyPanelTheme();

    StyleMixer m_mixer; // модель — вне виджетов
    QHash<QString, Row> m_rows;
    QLabel* m_sumLabel = nullptr;
    QPushButton* m_resetMixButton = nullptr;
    // Активная схема темы (см. Theme::panelStyleSheet).
    Theme::Scheme m_scheme = Theme::Scheme::Ptuch;
};
