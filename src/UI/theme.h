// theme.h
#pragma once

#include "suggestion/suggestion_controller.h" // SuggestionController::State

#include <QColor>
#include <QString>

class QPalette;

// Визуальная тема ПТЮЧ.AI — единый источник цветов, QPalette и
// stylesheet'ов для MainWindow, StylePanel и диалогов.
//
// Три схемы (ключ ui/style, комбобокс styleCombo в SettingsDialog):
//  - System ("default"): палитра и stylesheet'ы НЕ применяются —
//    цвета шрифтов и контролов системные (снимок палитры до темы
//    хранит MainWindow, см. applyTheme);
//  - Ptuch  ("ptuch"):   глубокий антрацит с тёмно-фиолетовым
//    подтоном, кислотно-жёлтый accent, электрик-синий secondary,
//    светлый текст;
//  - Light  ("light"):   светлые фоны, тёмный текст, те же accent'ы
//    в версиях, читаемых на светлом (accentInk, secondary).
// Контрастность проверяется тестом (tests/theme_test.cpp,
// WCAG-отношение яркости): основной текст >= 7:1, disabled >= 3:1,
// цвета состояний >= 4.5:1.
//
// Реализация — двумя средствами:
//  - QPalette: базовые роли для всех виджетов, включая те, что идут
//    без stylesheet; ghost-подсказка красит по Base/Text viewport'а,
//    поэтому живёт в тех же цветах, что и редактор;
//  - stylesheet: состояния контролов (hover / pressed / disabled /
//    focus), состояния контроллера (error / generating) на
//    индикаторе, фон редактора и панелей. Абсолютного позиционирования
//    нет — только layout'ы и правила (см. тест).
// Декоративные эффекты (скругления, градиент accent-кнопки) есть в
// Ptuch и Light; System снимает их вместе со стилями вообще.
namespace Theme {

// Схемы оформления (id в QSettings — см. AppSettings::style*).
enum class Scheme {
    System, // "default" — системные цвета шрифтов и контролов
    Ptuch,  // "ptuch"  — тёмная тема ПТЮЧ.AI
    Light,  // "light"  — светлая тема
};

// id схемы <-> перечисление; неизвестный/пустой id -> Scheme::Ptuch
// (страховка поверх AppSettings::sanitize).
Scheme schemeFromId(const QString& id);
QString schemeId(Scheme scheme);
// Названия пунктов комбобокса выбора стиля.
QString schemeTitle(Scheme scheme);

// Цвета схемы. Для Scheme::System ВСЕ поля невалидны: палитра и
// stylesheet'ы в этом случае не применяются — см. MainWindow.
struct Colors {
    QColor window;          // фон окна/статус-бара/заголовка dock
    QColor panel;           // панели, кнопки, диалоги (surface)
    QColor base;            // редактор и поля ввода (зона набора)
    QColor border;          // рамки
    QColor textColor;       // светлый/тёмный основной текст
    QColor labelColor;      // подписи
    QColor dimTextColor;    // вторичный текст (статус-бар, сноски)
    QColor disabledText;    // disabled: приглушённый, но читаемый
    QColor disabledBg;      // фон disabled-контрола
    QColor disabledBorder;  // рамка disabled-контрола
    QColor accent;          // accent-ЗАЛИВКА (CTA, выделение, меню)
    QColor accentHover;     // accent: hover
    QColor accentPressed;   // accent: pressed
    QColor accentText;      // тёмный текст поверх accent-заливки
    QColor accentInk;       // accent как МЕЛКИЙ ЭЛЕМЕНТ (рамки фокуса,
                            // маркер суммы, хэндл) — читается на фоне
    QColor secondary;       // электрик-синий: заливки, рамки, текст
    QColor secondaryDisabled; // выключенный включатель
    QColor hover;           // фон нейтральной кнопки в hover
    QColor pressed;         // фон нейтральной кнопки в pressed
    QColor success;         // состояние: готов
    QColor warning;         // состояние: ожидание (debounce)
    QColor error;           // состояние: ошибка
    // Стрелка-раскрытие комбобокса: SVG из ресурса (светлая на
    // тёмном фоне, тёмная на светлом). Для System пусто — стрелка
    // платформенная (stylesheet'ы не применяются).
    QString arrowImage;
};
Colors colors(Scheme scheme);

// QPalette приложения. Scheme::System -> пустая (не применять:
// возвращение к системной палитре делает MainWindow, см. applyTheme).
QPalette palette(Scheme scheme);

// Stylesheet'ы. Scheme::System -> пустая строка (контролы
// платформенные, включая шрифты). Состояния (hover/pressed/disabled/
// focus) и декор присутствуют в Ptuch/Light.
QString mainStyleSheet(Scheme scheme);
QString panelStyleSheet(Scheme scheme);
QString dialogStyleSheet(Scheme scheme);

// Цветовой маркер StylePanel под профиль: цвет профиля (+ рамка и
// радиус схемы; у System — нейтральная рамка без радиуса).
QString colorMarkerStyle(const QColor& profileColor, Scheme scheme);

// Состояния контроллера: заголовок индикатора и его цвет.
// error — красный, generating — электрик-синий и т.д.; для
// Scheme::System цвет НЕ возвращается (надпись остаётся
// системной — проверяется tests/theme_test.cpp).
QString stateTitle(SuggestionController::State state);
QColor stateColor(Scheme scheme, SuggestionController::State state);

} // namespace Theme
