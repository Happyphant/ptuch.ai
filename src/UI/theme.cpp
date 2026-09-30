// theme.cpp
#include "theme.h"

#include "settings/app_settings.h" // AppSettings::style* (id схем)

#include <QPalette>

namespace {

// ---------------------------------------------------------------------------
// Подстановка токенов в stylesheet-заготовки.
//
// Порядок важен: сначала декоративные токены (@RAD*@, @GRAD@) — они
// вливают цветовые литералы (@ACCENT*@), затем цвета схемы из
// Theme::colors (единый источник для stylesheet, палитры и меток).
// Схема System сюда не доходит: её stylesheet'ы пустые (см. ниже).
// ---------------------------------------------------------------------------
QString resolve(QString sheet, const Theme::Colors& colors)
{
    sheet.replace(QStringLiteral("@RAD2@"),
                  QStringLiteral("border-radius: 2px;"));
    sheet.replace(QStringLiteral("@RAD3@"),
                  QStringLiteral("border-radius: 3px;"));
    sheet.replace(QStringLiteral("@RAD4@"),
                  QStringLiteral("border-radius: 4px;"));
    sheet.replace(QStringLiteral("@RAD6@"),
                  QStringLiteral("border-radius: 6px;"));
    sheet.replace(QStringLiteral("@RAD7@"),
                  QStringLiteral("border-radius: 7px;"));
    // Градиент accent-кнопки — единственный градиент темы.
    sheet.replace(
        QStringLiteral("@GRAD@"),
        QStringLiteral(
            "background: qlineargradient(x1: 0, y1: 0, x2: 0, "
            "y2: 1, stop: 0 @ACCENT_HOVER@, stop: 1 @ACCENT@);"));

    sheet.replace(QStringLiteral("@WINDOW@"), colors.window.name());
    sheet.replace(QStringLiteral("@PANEL@"), colors.panel.name());
    sheet.replace(QStringLiteral("@BASE@"), colors.base.name());
    sheet.replace(QStringLiteral("@BORDER@"), colors.border.name());
    sheet.replace(QStringLiteral("@TEXT@"), colors.textColor.name());
    sheet.replace(QStringLiteral("@LABEL@"), colors.labelColor.name());
    sheet.replace(QStringLiteral("@DIM@"), colors.dimTextColor.name());
    sheet.replace(QStringLiteral("@DISABLED_TEXT@"),
                  colors.disabledText.name());
    sheet.replace(QStringLiteral("@DISABLED_BG@"),
                  colors.disabledBg.name());
    sheet.replace(QStringLiteral("@DISABLED_BORDER@"),
                  colors.disabledBorder.name());
    sheet.replace(QStringLiteral("@ACCENT_HOVER@"),
                  colors.accentHover.name());
    sheet.replace(QStringLiteral("@ACCENT_PRESSED@"),
                  colors.accentPressed.name());
    sheet.replace(QStringLiteral("@ACCENT_TEXT@"),
                  colors.accentText.name());
    sheet.replace(QStringLiteral("@ACCENT_INK@"),
                  colors.accentInk.name());
    sheet.replace(QStringLiteral("@ACCENT@"), colors.accent.name());
    sheet.replace(QStringLiteral("@SECONDARY_DISABLED@"),
                  colors.secondaryDisabled.name());
    sheet.replace(QStringLiteral("@SECONDARY@"),
                  colors.secondary.name());
    sheet.replace(QStringLiteral("@HOVER@"), colors.hover.name());
    sheet.replace(QStringLiteral("@PRESSED@"),
                  colors.pressed.name());
    sheet.replace(QStringLiteral("@ARROW@"), colors.arrowImage);
    return sheet;
}

// Нейтральные кнопки: фон панели; hover — фон светлее и рамка
// электрик-синим; pressed — темнее; disabled — тище; focus — рамка
// accent-ink (на светлом жёлтый accent как рамка не читается, ink
// обеих схем читается; у жёлтого CTA фокус синий — см. main sheet).
QString neutralButtonRules(const Theme::Colors& colors)
{
    return resolve(QStringLiteral(R"(
        QPushButton {
            background: @PANEL@;
            color: @TEXT@;
            border: 1px solid @BORDER@;
            @RAD4@
            padding: 6px 12px;
        }

        QPushButton:hover {
            background: @HOVER@;
            border-color: @SECONDARY@;
        }

        QPushButton:pressed {
            background: @PRESSED@;
            border-color: @SECONDARY@;
        }

        QPushButton:disabled {
            background: @DISABLED_BG@;
            color: @DISABLED_TEXT@;
            border-color: @DISABLED_BORDER@;
        }

        QPushButton:focus {
            border-color: @ACCENT_INK@;
        }
    )"),
                  colors);
}

// Поля ввода (в т.ч. зона набора формы): тот же фон, что у редактора.
QString inputRules(const Theme::Colors& colors)
{
    return resolve(QStringLiteral(R"(
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background-color: @BASE@;
            color: @TEXT@;
            border: 1px solid @BORDER@;
            @RAD4@
            padding: 4px 6px;
            selection-background-color: @ACCENT@;
            selection-color: @ACCENT_TEXT@;
        }

        QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover,
        QComboBox:hover {
            border-color: @SECONDARY@;
        }

        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus,
        QComboBox:focus {
            border-color: @ACCENT_INK@;
        }

        QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled,
        QComboBox:disabled {
            background-color: @DISABLED_BG@;
            color: @DISABLED_TEXT@;
            border-color: @DISABLED_BORDER@;
        }

        /* Классический dropdown под комбобоксом. Без этого Qt
           позиционирует popup «менюшкой»: список выравнивается по
           выбранному пункту и в темизированных стилях уезжает вверх
           мимо комбобокса (на величину высоты строк). */
        QComboBox {
            combobox-popup: 0;
        }

        QComboBox::drop-down {
            border: none;
            background: transparent;
        }

        /* Стрелка задаётся явным изображением: без правила
           QComboBox::down-arrow стиль рисует её системным
           контрастом и на фоне темы она не видна. */
        QComboBox::down-arrow {
            image: url(@ARROW@);
            width: 10px;
            height: 6px;
        }

        QComboBox QAbstractItemView {
            background-color: @PANEL@;
            color: @TEXT@;
            border: 1px solid @BORDER@;
            selection-background-color: @ACCENT@;
            selection-color: @ACCENT_TEXT@;
        }
    )"),
                  colors);
}

// Чекбоксы: индикатор в цвет зоны ввода; включён — электрик-синий;
// hover — рамка синим; disabled — тище.
QString checkBoxRules(const Theme::Colors& colors)
{
    return resolve(QStringLiteral(R"(
        QCheckBox {
            color: @LABEL@;
            spacing: 6px;
            background: transparent;
        }

        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            background-color: @BASE@;
            border: 1px solid @BORDER@;
            @RAD3@
        }

        QCheckBox::indicator:hover {
            border-color: @SECONDARY@;
        }

        QCheckBox::indicator:checked {
            background-color: @SECONDARY@;
            border: 1px solid @SECONDARY@;
        }

        QCheckBox::indicator:disabled {
            background-color: @DISABLED_BG@;
            border-color: @DISABLED_BORDER@;
        }

        QCheckBox::indicator:checked:disabled {
            background-color: @SECONDARY_DISABLED@;
            border-color: @DISABLED_BORDER@;
        }

        QCheckBox:disabled {
            color: @DISABLED_TEXT@;
        }
    )"),
                  colors);
}

} // namespace

namespace Theme {

// --- Схемы -----------------------------------------------------------------

Scheme schemeFromId(const QString& id)
{
    if (id == QLatin1String(AppSettings::styleSystem))
        return Scheme::System;
    if (id == QLatin1String(AppSettings::styleLight))
        return Scheme::Light;
    // Неизвестный/пустой id -> тёмная тема ПТЮЧ (как в sanitize()).
    return Scheme::Ptuch;
}

QString schemeId(Scheme scheme)
{
    switch (scheme) {
    case Scheme::System:
        return QLatin1String(AppSettings::styleSystem);
    case Scheme::Ptuch:
        return QLatin1String(AppSettings::stylePtuch);
    case Scheme::Light:
        return QLatin1String(AppSettings::styleLight);
    }
    return {};
}

QString schemeTitle(Scheme scheme)
{
    switch (scheme) {
    case Scheme::System:
        return QStringLiteral("default");
    case Scheme::Ptuch:
        return QStringLiteral("ПТЮЧ");
    case Scheme::Light:
        return QStringLiteral("светлый");
    }
    return {};
}

// --- Цвета схем ------------------------------------------------------------

Colors colors(Scheme scheme)
{
    Colors value; // для System — все поля невалидны (см. ниже)

    switch (scheme) {
    case Scheme::System:
        // Системная схема: цветов нет — палитра/stylesheet'ы не
        // применяются, контролы используют платформенные значения.
        break;

    case Scheme::Ptuch:
        // Антрацит с тёмно-фиолетовым подтоном; accent и secondary —
        // яркие: читаются на тёмном фоне.
        value.window = QColor(QStringLiteral("#14131a"));
        value.panel = QColor(QStringLiteral("#1e1c29"));
        value.base = QColor(QStringLiteral("#17161f"));
        value.border = QColor(QStringLiteral("#34314a"));
        value.textColor = QColor(QStringLiteral("#eceaf4"));
        value.labelColor = QColor(QStringLiteral("#d6d2e6"));
        value.dimTextColor = QColor(QStringLiteral("#a6a1bd"));
        value.disabledText = QColor(QStringLiteral("#757089"));
        value.disabledBg = QColor(QStringLiteral("#1a1826"));
        value.disabledBorder = QColor(QStringLiteral("#2a2739"));
        value.accent = QColor(QStringLiteral("#e8ff3a"));
        value.accentHover = QColor(QStringLiteral("#f2ff66"));
        value.accentPressed = QColor(QStringLiteral("#c9dd00"));
        value.accentText = QColor(QStringLiteral("#16150c"));
        value.accentInk = value.accent; // жёлтый читается на тёмном
        value.secondary = QColor(QStringLiteral("#38b6ff"));
        value.secondaryDisabled = QColor(QStringLiteral("#2a5f8f"));
        value.hover = QColor(QStringLiteral("#292641"));
        value.pressed = QColor(QStringLiteral("#151322"));
        value.success = QColor(QStringLiteral("#3ddc84"));
        value.warning = QColor(QStringLiteral("#ffb300"));
        value.error = QColor(QStringLiteral("#ff5c5c"));
        value.arrowImage =
            QStringLiteral(":/theme/arrow-down-on-dark.svg");
        break;

    case Scheme::Light:
        // Светлые фоны с фиолетовым подтоном; accent — та же
        // кислотная жёлтость (заливки), accentInk/secondary —
        // тёмные версии для текста и рамок на светлом.
        value.window = QColor(QStringLiteral("#e9e7f0"));
        value.panel = QColor(QStringLiteral("#f6f5fa"));
        value.base = QColor(QStringLiteral("#ffffff"));
        value.border = QColor(QStringLiteral("#c6c1d6"));
        value.textColor = QColor(QStringLiteral("#1b1926"));
        value.labelColor = QColor(QStringLiteral("#322e42"));
        value.dimTextColor = QColor(QStringLiteral("#5f5a78"));
        value.disabledText = QColor(QStringLiteral("#7c7791"));
        value.disabledBg = QColor(QStringLiteral("#e6e3ee"));
        value.disabledBorder = QColor(QStringLiteral("#d3cfdd"));
        value.accent = QColor(QStringLiteral("#e8ff3a"));
        value.accentHover = QColor(QStringLiteral("#f2ff66"));
        value.accentPressed = QColor(QStringLiteral("#c9dd00"));
        value.accentText = QColor(QStringLiteral("#16150c"));
        // Тёмная «чернильная» версия жёлтого: рамки фокуса и мелкий
        // акцентный текст на светлом (контраст >= 5:1).
        value.accentInk = QColor(QStringLiteral("#556200"));
        // Электрик-синий, затемнённый до читаемого на светлом.
        value.secondary = QColor(QStringLiteral("#0b63c5"));
        value.secondaryDisabled = QColor(QStringLiteral("#7fa9d4"));
        value.hover = QColor(QStringLiteral("#e6e2f2"));
        value.pressed = QColor(QStringLiteral("#d8d3ea"));
        value.success = QColor(QStringLiteral("#157347"));
        value.warning = QColor(QStringLiteral("#8f5100"));
        value.error = QColor(QStringLiteral("#c62828"));
        value.arrowImage =
            QStringLiteral(":/theme/arrow-down-on-light.svg");
        break;
    }

    return value;
}

QPalette palette(Scheme scheme)
{
    QPalette value; // System -> пустая: применять нельзя (см. хедер)

    if (scheme == Scheme::System)
        return value;

    const Colors c = colors(scheme);

    value.setColor(QPalette::Window, c.window);
    value.setColor(QPalette::WindowText, c.textColor);
    value.setColor(QPalette::Base, c.base);
    value.setColor(QPalette::AlternateBase, c.panel);
    value.setColor(QPalette::ToolTipBase, c.panel);
    value.setColor(QPalette::ToolTipText, c.textColor);
    value.setColor(QPalette::Text, c.textColor);
    value.setColor(QPalette::Button, c.panel);
    value.setColor(QPalette::ButtonText, c.textColor);
    value.setColor(QPalette::BrightText, c.error);
    value.setColor(QPalette::Link, c.secondary);
    value.setColor(QPalette::LinkVisited, c.secondary);
    value.setColor(QPalette::Light, c.textColor);
    value.setColor(QPalette::Midlight, c.labelColor);
    value.setColor(QPalette::Mid, c.border);
    value.setColor(QPalette::Dark, c.border);
    value.setColor(QPalette::Shadow, QColor(Qt::black));
    // Выделение — accent-заливка, текст на нём тёмный.
    value.setColor(QPalette::Highlight, c.accent);
    value.setColor(QPalette::HighlightedText, c.accentText);
    value.setColor(QPalette::PlaceholderText, c.dimTextColor);

    // Disabled: заметно тише обычного, но читаемо на фоне окна
    // (контраст >= 3:1 — проверяется tests/theme_test.cpp).
    value.setColor(QPalette::Disabled, QPalette::WindowText,
                   c.disabledText);
    value.setColor(QPalette::Disabled, QPalette::Text,
                   c.disabledText);
    value.setColor(QPalette::Disabled, QPalette::ButtonText,
                   c.disabledText);
    value.setColor(QPalette::Disabled, QPalette::BrightText,
                   c.disabledText);
    value.setColor(QPalette::Disabled, QPalette::Link,
                   c.disabledText);
    value.setColor(QPalette::Disabled, QPalette::ToolTipText,
                   c.disabledText);
    value.setColor(QPalette::Disabled, QPalette::Highlight,
                   c.disabledBorder);
    value.setColor(QPalette::Disabled, QPalette::HighlightedText,
                   c.disabledText);

    return value;
}

// --- Stylesheet'ы ---------------------------------------------------------

QString mainStyleSheet(Scheme scheme)
{
    // Системная схема: ничего не стилизуем — шрифты и контролы
    // платформенные (включая зону набора).
    if (scheme == Scheme::System)
        return {};

    const Colors c = colors(scheme);

    return resolve(QStringLiteral(R"(
        QMainWindow {
            background-color: @WINDOW@;
        }

        /* Базовый текст: контрастный к фону схемы. */
        QWidget {
            color: @TEXT@;
            font-size: 13px;
        }

        QToolBar#statusPanel {
            background-color: @PANEL@;
            border: none;
            border-bottom: 1px solid @BORDER@;
            spacing: 8px;
            padding: 4px;
        }

        /* Индикатор состояния: цвет текста (Ready/Waiting/Generating/
           Error) задаёт MainWindow::updateStateIndicator из
           Theme::stateColor — красный здесь только при ошибке,
           синий — при генерации (см. tests/theme_test.cpp). */
        QLabel#stateIndicator {
            background-color: @BASE@;
            border: 1px solid @BORDER@;
            @RAD4@
            padding: 5px 10px;
            font-weight: bold;
        }

        /* Primary CTA — accent-заливка: градиент, hover/pressed
           перекрывают её тем же свойством background. */
        QPushButton#generateButton {
            @GRAD@
            color: @ACCENT_TEXT@;
            border: 1px solid @ACCENT@;
            @RAD4@
            padding: 6px 14px;
            font-weight: bold;
        }

        QPushButton#generateButton:hover {
            background: @ACCENT_HOVER@;
            border-color: @ACCENT_HOVER@;
        }

        QPushButton#generateButton:pressed {
            background: @ACCENT_PRESSED@;
            border-color: @ACCENT_PRESSED@;
        }

        QPushButton#generateButton:disabled {
            background: @DISABLED_BG@;
            color: @DISABLED_TEXT@;
            border-color: @DISABLED_BORDER@;
        }

        /* Фокус на жёлтом CTA — рамка электрик-синим: жёлтая на
           жёлтом не читается, синяя — вторичный accent темы. */
        QPushButton#generateButton:focus {
            border-color: @SECONDARY@;
        }

        %NEUTRAL_BUTTONS%

        %INPUTS%

        /* Область набора: плоская — без рамок и скруглений и без
           :focus-обводки (визуального шума при наборе нет); выделение
           — accent-заливкой. Background совпадает с Base палитры,
           ghost-подсказка красит по ней же. */
        QPlainTextEdit#mainEditor {
            background-color: @BASE@;
            color: @TEXT@;
            border: none;
            padding: 8px;
            selection-background-color: @ACCENT@;
            selection-color: @ACCENT_TEXT@;
        }

        QLabel {
            color: @LABEL@;
        }

        QStatusBar {
            background-color: @WINDOW@;
            color: @DIM@;
            border-top: 1px solid @BORDER@;
        }

        QStatusBar::item {
            border: none;
        }

        QDockWidget#styleDock {
            background-color: @PANEL@;
            color: @TEXT@;
        }

        QDockWidget#styleDock::title {
            background-color: @WINDOW@;
            color: @DIM@;
            text-align: left;
            padding: 6px;
            border-bottom: 1px solid @BORDER@;
        }

        QMenu {
            background-color: @PANEL@;
            color: @TEXT@;
            border: 1px solid @BORDER@;
            @RAD4@
            padding: 4px;
        }

        QMenu::item {
            padding: 6px 24px 6px 12px;
        }

        QMenu::item:selected {
            background-color: @ACCENT@;
            color: @ACCENT_TEXT@;
        }

        QMenu::separator {
            background-color: @BORDER@;
            height: 1px;
            margin: 4px 8px;
        }

        QToolTip {
            background-color: @PANEL@;
            color: @TEXT@;
            border: 1px solid @BORDER@;
            padding: 4px;
        }
    )")
                  .replace(QStringLiteral("%NEUTRAL_BUTTONS%"),
                           neutralButtonRules(c))
                  .replace(QStringLiteral("%INPUTS%"), inputRules(c)),
                  c);
}

QString panelStyleSheet(Scheme scheme)
{
    if (scheme == Scheme::System)
        return {};

    const Colors c = colors(scheme);

    return resolve(QStringLiteral(R"(
        StylePanel {
            background-color: @PANEL@;
            border: none;
        }

        QLabel {
            color: @LABEL@;
            background: transparent;
        }

        QLabel#sumLabel {
            color: @ACCENT_INK@;
            font-weight: bold;
        }

        QSlider::groove:horizontal {
            height: 4px;
            background: @BORDER@;
            @RAD2@
        }

        QSlider::sub-page:horizontal {
            background: @SECONDARY@;
            @RAD2@
        }

        QSlider::handle:horizontal {
            background: @TEXT@;
            border: 1px solid @SECONDARY@;
            width: 12px;
            margin: -5px 0;
            @RAD6@
        }

        QSlider::handle:horizontal:hover {
            background: @ACCENT_INK@;
            border-color: @ACCENT_INK@;
        }

        /* Выключенный стиль: слайдер гаснет, но остаётся читаемым. */
        QSlider::groove:horizontal:disabled {
            background: @DISABLED_BORDER@;
        }

        QSlider::sub-page:horizontal:disabled {
            background: @SECONDARY_DISABLED@;
        }

        QSlider::handle:horizontal:disabled {
            background: @DISABLED_TEXT@;
            border: 1px solid @DISABLED_BORDER@;
        }

        %CHECK_BOXES%

        QPushButton#resetMixButton,
        QPushButton[kind="styleReset"] {
            background: @PANEL@;
            color: @TEXT@;
            border: 1px solid @BORDER@;
            @RAD4@
            padding: 4px 8px;
        }

        QPushButton#resetMixButton:hover,
        QPushButton[kind="styleReset"]:hover {
            background: @HOVER@;
            border-color: @SECONDARY@;
        }

        QPushButton#resetMixButton:pressed,
        QPushButton[kind="styleReset"]:pressed {
            background: @PRESSED@;
            border-color: @SECONDARY@;
        }

        QPushButton#resetMixButton:disabled,
        QPushButton[kind="styleReset"]:disabled {
            background: @DISABLED_BG@;
            color: @DISABLED_TEXT@;
            border-color: @DISABLED_BORDER@;
        }

        QPushButton#resetMixButton:focus,
        QPushButton[kind="styleReset"]:focus {
            border-color: @ACCENT_INK@;
        }
    )")
                  .replace(QStringLiteral("%CHECK_BOXES%"),
                           checkBoxRules(c)),
                  c);
}

QString dialogStyleSheet(Scheme scheme)
{
    if (scheme == Scheme::System)
        return {};

    const Colors c = colors(scheme);

    return resolve(QStringLiteral(R"(
        QDialog {
            background-color: @PANEL@;
            border: none;
        }

        QLabel {
            color: @LABEL@;
        }

        %INPUTS%

        %NEUTRAL_BUTTONS%

        %CHECK_BOXES%
    )")
                  .replace(QStringLiteral("%INPUTS%"), inputRules(c))
                  .replace(QStringLiteral("%NEUTRAL_BUTTONS%"),
                           neutralButtonRules(c))
                  .replace(QStringLiteral("%CHECK_BOXES%"),
                           checkBoxRules(c)),
                  c);
}

QString colorMarkerStyle(const QColor& profileColor, Scheme scheme)
{
    // Системная схема: цвет профиля — данные, он обязан быть виден,
    // но без декора: нейтральная рамка, без радиуса.
    if (scheme == Scheme::System) {
        return QStringLiteral(
                   "background-color: %1; border: 1px solid "
                   "rgba(0, 0, 0, 64);")
            .arg(profileColor.name());
    }

    return resolve(QStringLiteral(R"(
        background-color: %PROFILE%;
        border: 1px solid @BORDER@;
        @RAD7@
    )"),
                   colors(scheme))
        .replace(QStringLiteral("%PROFILE%"), profileColor.name());
}

// --- Состояния контроллера ------------------------------------------------

QString stateTitle(SuggestionController::State state)
{
    using State = SuggestionController::State;

    switch (state) {
    case State::Idle:
        return QStringLiteral("Ready");
    case State::Debouncing:
        return QStringLiteral("Waiting");
    case State::Generating:
        return QStringLiteral("Generating");
    case State::Ready:
        return QStringLiteral("Ready");
    case State::Error:
        return QStringLiteral("Error");
    }
    return {};
}

QColor stateColor(Scheme scheme, SuggestionController::State state)
{
    // Системная схема: цвет состояния не задаём — надпись остаётся
    // со системным шрифтом (состояние читается по заголовку).
    if (scheme == Scheme::System)
        return {};

    using State = SuggestionController::State;
    const Colors c = colors(scheme);

    switch (state) {
    case State::Idle:
        return c.success;
    case State::Debouncing:
        return c.warning;
    case State::Generating:
        return c.secondary;
    case State::Ready:
        return c.success;
    case State::Error:
        return c.error;
    }
    return c.textColor;
}

} // namespace Theme
