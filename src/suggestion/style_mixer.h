// style_mixer.h
#pragma once

#include "backend/text_generation_backend.h" // StyleWeight
#include "suggestion/style_profile.h"

#include <QString>
#include <QVector>

// Микшер стилевых профилей: хранит список StyleProfile и превращает
// его в (а) нормализованные веса для запроса и (б) компактную
// prompt-инструкцию для модели.
//
// Инварианты (правила из ТЗ):
//  1. Сумма активных весов предсказуема: любые выходные формы
//     (styleWeights(), buildInstruction(), normalize()) используют
//     нормализацию «вес / сумма активных» — сумма ровно 1.0
//     (с точностью до float).
//  2. Нулевой профиль (weight == 0) и выключенный (enabled == false)
//     не попадают ни в веса, ни в prompt.
//  3. Prompt не разрастается: buildInstruction() — чистая функция
//     текущего состояния, по одной строке на активный профиль; от
//     числа событий UI (вызовов) результат не зависит.
//  4. Только Qt Core/Gui-типы (QString/QVector/QColor) — никакой
//     зависимости от Qt-виджетов; объект не владеет сигналами и
//     таймерами, тесты собираются с QTEST_GUILESS_MAIN.
//
// Интеграция: styleWeights() кормит SuggestionController::setStyleMix
// (захват в GenerationRequest::styleWeights) — llama-backend пока
// логирует веса, использование в prompt — отдельная задача.
class StyleMixer final
{
public:
    // Встроенные профили: Пушкин, Тютчев, «Киберпанк 80-х»,
    // «Официальный стиль» (см. юридическую оговорку в style_profile.h).
    StyleMixer();
    // Свой список профилей: порядок сохраняется, веса санируются в
    // [0; 1] при конструировании.
    explicit StyleMixer(QVector<StyleProfile> profiles);

    const QVector<StyleProfile>& profiles() const { return m_profiles; }
    // Профиль по id либо nullptr (id не найден).
    const StyleProfile* profile(const QString& id) const;

    // Установка веса (кламп [0; 1]) / флага включения по id.
    // false, если id не найден.
    bool setWeight(const QString& id, double weight);
    bool setEnabled(const QString& id, bool enabled);

    // Нормализация на месте: веса АКТИВНЫХ профилей масштабируются
    // так, чтобы их сумма стала ровно 1.0; неактивные не трогаются.
    // Идемпотентно. (Для выходных форм нормализация считается на лету
    // — вызов не обязателен.)
    void normalize();

    // Активные профили: enabled && weight > 0, в порядке списка.
    QVector<StyleProfile> activeProfiles() const;

    // Нормализованные веса активных профилей (сумма == 1.0) в формате
    // GenerationRequest::styleWeights — готово к setStyleMix().
    // Пусто, если активных нет.
    QVector<StyleWeight> styleWeights() const;

    // Компактная инструкция для модели: по строке на активный профиль
    // вида «<имя> (<нормализованный вес>): <инструкция>».
    // Неактивные и профили без текста инструкции не попадают; при
    // отсутствии активных — пустая строка.
    QString buildInstruction() const;

    // Сумма весов активных профилей (0.0, если активных нет) — для
    // отображения в UI (StylePanel): предсказуемая величина, см.
    // комментарий к normalize().
    double activeTotal() const;

private:
    QVector<StyleProfile> m_profiles;
};
