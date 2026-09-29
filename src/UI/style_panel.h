// style_panel.h
#pragma once

#include "suggestion/style_mixer.h"

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
// Тёмная тема: панель приносит собственный stylesheet (тёмный фон,
// светлый текст, акцент #42a5f5) — корректно выглядит и внутри
// MainWindow, и отдельно от него.
class StylePanel final : public QWidget
{
    Q_OBJECT

public:
    explicit StylePanel(QWidget* parent = nullptr);
    // DI: свой микшер (например, восстановленный из настроек).
    explicit StylePanel(StyleMixer mixer, QWidget* parent = nullptr);

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

    StyleMixer m_mixer; // модель — вне виджетов
    QHash<QString, Row> m_rows;
    QLabel* m_sumLabel = nullptr;
    QPushButton* m_resetMixButton = nullptr;
};
