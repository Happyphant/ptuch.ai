// theme_test.cpp
#include <QtTest>

#include "UI/theme.h"
#include "settings/app_settings.h"

#include <QPalette>

#include <cmath>

namespace {

// WCAG-отношение яркостей (relative luminance) — проверка контрастности.
double channelLuminance(int value)
{
    const double v = double(value) / 255.0;
    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& color)
{
    return 0.2126 * channelLuminance(color.red())
           + 0.7152 * channelLuminance(color.green())
           + 0.0722 * channelLuminance(color.blue());
}

// WCAG-отношение яркостей двух цветов (1.0..21.0).
double contrast(const QColor& first, const QColor& second)
{
    const double a = luminance(first);
    const double b = luminance(second);
    const double lighter = qMax(a, b);
    const double darker = qMin(a, b);
    return (lighter + 0.05) / (darker + 0.05);
}

// Сравнение по компонентам: спецификация QColor (Rgb/Hsl) может
// расходиться при равных цветах.
bool sameColor(const QColor& first, const QColor& second)
{
    return first.red() == second.red() && first.green() == second.green()
           && first.blue() == second.blue();
}

// Ни один stylesheet темы не должен содержать незамещённых токенов.
bool hasNoLeftoverTokens(const QString& sheet)
{
    return !sheet.contains(QLatin1Char('@'));
}

// У системной схемы все поля цветов невалидны: палитра и
// stylesheet'ы не применяются.
bool allColorsInvalid(const Theme::Colors& colors)
{
    const QList<QColor> all = {
        colors.window,      colors.panel,        colors.base,
        colors.border,      colors.textColor,    colors.labelColor,
        colors.dimTextColor, colors.disabledText, colors.disabledBg,
        colors.disabledBorder, colors.accent,    colors.accentHover,
        colors.accentPressed, colors.accentText,  colors.accentInk,
        colors.secondary,   colors.secondaryDisabled, colors.hover,
        colors.pressed,     colors.success,      colors.warning,
        colors.error,
    };
    for (const QColor& color : all) {
        if (color.isValid())
            return false;
    }
    return true;
}

// Правило селектора: от него до первой закрывающей скобки.
QString firstRule(const QString& sheet, const QString& selector)
{
    const int begin = sheet.indexOf(selector);
    if (begin < 0)
        return {};
    const int end = sheet.indexOf(QLatin1Char('}'), begin);
    return end > begin ? sheet.mid(begin, end - begin) : QString();
}

} // namespace

// ---------------------------------------------------------------------------
// Тема ПТЮЧ.AI (src/UI/theme.*): три схемы — default (системная),
// ПТЮЧ (тёмная) и светлая:
//  1) ид-схем: roundtrip, мусорный id -> ПТЮЧ (как в sanitize);
//  2) палитры Ptuch/Light: контрастность по WCAG, accent/secondary в
//     ролях выделения/ссылок; у System все цветы невалидны;
//  3) stylesheet'ы главного окна покрывают hover/pressed/disabled/
//     focus, зона набора — плоская и без :focus-обводки;
//  4) состояния контроллера: error — красный, generating —
//     электрик-синий, читаются на фоне своей схемы; System — без цвета;
//  5) System вообще ничего не стилизует; Ptuch/Light — декор есть;
//  6) панель и диалоги используют те же цвета схемы.
// ---------------------------------------------------------------------------
class ThemeTest final : public QObject
{
    Q_OBJECT

private slots:
    void schemeIdsRoundtripAndUnknownBecomesPtuch();
    void palettesAreHighContrastPerScheme();
    void mainStylesheetCoversInteractiveStates();
    void stateColorsDistinguishErrorAndGenerating();
    void systemSchemeOmitsAllStyling();
    void panelAndDialogSheetsShareThemeColors();
};

void ThemeTest::schemeIdsRoundtripAndUnknownBecomesPtuch()
{
    // id <-> схема: те же строки, что в QSettings (AppSettings::style*).
    QCOMPARE(Theme::schemeId(Theme::Scheme::System),
             QLatin1String(AppSettings::styleSystem));
    QCOMPARE(Theme::schemeId(Theme::Scheme::Ptuch),
             QLatin1String(AppSettings::stylePtuch));
    QCOMPARE(Theme::schemeId(Theme::Scheme::Light),
             QLatin1String(AppSettings::styleLight));
    QCOMPARE(Theme::schemeFromId(QStringLiteral("default")),
             Theme::Scheme::System);
    QCOMPARE(Theme::schemeFromId(QStringLiteral("ptuch")),
             Theme::Scheme::Ptuch);
    QCOMPARE(Theme::schemeFromId(QStringLiteral("light")),
             Theme::Scheme::Light);

    // Мусорный/пустой id -> тёмная тема ПТЮЧ (страховка поверх
    // AppSettings::sanitize — поведение совпадает).
    QCOMPARE(Theme::schemeFromId(QString()), Theme::Scheme::Ptuch);
    QCOMPARE(Theme::schemeFromId(QStringLiteral("нет")),
             Theme::Scheme::Ptuch);

    // Названия пунктов комбобокса: ровно три, все различимы.
    QCOMPARE(Theme::schemeTitle(Theme::Scheme::System),
             QStringLiteral("default"));
    QCOMPARE(Theme::schemeTitle(Theme::Scheme::Ptuch),
             QStringLiteral("ПТЮЧ"));
    QCOMPARE(Theme::schemeTitle(Theme::Scheme::Light),
             QStringLiteral("светлый"));
    QVERIFY(Theme::schemeTitle(Theme::Scheme::Ptuch)
            != Theme::schemeTitle(Theme::Scheme::Light));
}

void ThemeTest::palettesAreHighContrastPerScheme()
{
    // --- ПТЮЧ (тёмная): антрацит/фиолетовый фон, светлый текст ------
    const Theme::Colors dark = Theme::colors(Theme::Scheme::Ptuch);
    QVERIFY(luminance(dark.window) < 0.1);
    QVERIFY(luminance(dark.base) < 0.1);
    QVERIFY(luminance(dark.base) >= luminance(dark.window));
    QVERIFY(luminance(dark.textColor) > 0.5);
    QVERIFY(contrast(dark.textColor, dark.base) >= 7.0);
    QVERIFY(contrast(dark.textColor, dark.window) >= 7.0);
    QVERIFY(contrast(dark.disabledText, dark.window) >= 3.0);
    QVERIFY(luminance(dark.disabledText) < luminance(dark.textColor));

    // --- Светлая: светлый фон, тёмный текст, те же пороги -----------
    const Theme::Colors light = Theme::colors(Theme::Scheme::Light);
    QVERIFY(luminance(light.window) > 0.5);
    QVERIFY(luminance(light.base) > 0.5);
    QVERIFY(luminance(light.textColor) < 0.2);
    QVERIFY(contrast(light.textColor, light.base) >= 7.0);
    QVERIFY(contrast(light.textColor, light.window) >= 7.0);
    QVERIFY(contrast(light.disabledText, light.window) >= 3.0);
    QVERIFY(luminance(light.disabledText) > luminance(light.textColor));

    // --- Роли палитры (обе темизированные схемы) --------------------
    for (const Theme::Scheme scheme :
         {Theme::Scheme::Ptuch, Theme::Scheme::Light}) {
        const Theme::Colors c = Theme::colors(scheme);
        const QPalette pal = Theme::palette(scheme);

        // Выделение — accent-заливка, текст на нём тёмный и читаем.
        QVERIFY(sameColor(pal.color(QPalette::Highlight), c.accent));
        QVERIFY(contrast(pal.color(QPalette::HighlightedText),
                         pal.color(QPalette::Highlight)) >= 7.0);
        // Ссылки — электрик-синий (readable-версия схемы).
        QVERIFY(sameColor(pal.color(QPalette::Link), c.secondary));
        QVERIFY(sameColor(pal.color(QPalette::PlaceholderText),
                          c.dimTextColor));
        QVERIFY(sameColor(pal.color(QPalette::Text), c.textColor));
        QVERIFY(sameColor(pal.color(QPalette::Base), c.base));
        // Disabled: приглушённый, но читаемый на фоне окна.
        const QColor disabled =
            pal.color(QPalette::Disabled, QPalette::Text);
        QVERIFY(sameColor(disabled, c.disabledText));
        QVERIFY(contrast(disabled, c.window) >= 3.0);
    }

    // --- default: цветов нет — применяется системная палитра -------
    QVERIFY(allColorsInvalid(Theme::colors(Theme::Scheme::System)));
    QVERIFY(!Theme::stateColor(Theme::Scheme::System,
                               SuggestionController::State::Error)
                 .isValid());
}

void ThemeTest::mainStylesheetCoversInteractiveStates()
{
    for (const Theme::Scheme scheme :
         {Theme::Scheme::Ptuch, Theme::Scheme::Light}) {
        const QString sheet = Theme::mainStyleSheet(scheme);
        QVERIFY(!sheet.isEmpty());
        QVERIFY2(hasNoLeftoverTokens(sheet),
                 "Stylesheet главного окна содержит незамещённые токены");

        // Состояния контролов присутствуют явно.
        QVERIFY(sheet.contains(QStringLiteral(":hover")));
        QVERIFY(sheet.contains(QStringLiteral(":pressed")));
        QVERIFY(sheet.contains(QStringLiteral(":disabled")));
        QVERIFY(sheet.contains(QStringLiteral(":focus")));

        // Цвета темы — те же, что в colors(): фон редактора совпадает
        // с Base палитры (ghost-подсказка красит по палитре).
        const Theme::Colors c = Theme::colors(scheme);
        QVERIFY(sheet.contains(c.window.name()));
        QVERIFY(sheet.contains(c.base.name()));
        QVERIFY(sheet.contains(c.accent.name()));
        QVERIFY(sheet.contains(c.secondary.name()));

        // Комбобокс: явная стрелка-изображение (без правила она
        // рисуется системным контрастом и на фоне темы не видна) и
        // классический dropdown (иначе Qt позиционирует список
        // «менюшкой» по выбранному пункту — вверх мимо комбобокса).
        QVERIFY(sheet.contains(QStringLiteral("QComboBox::down-arrow")));
        QVERIFY(sheet.contains(QStringLiteral("combobox-popup: 0")));
        QVERIFY(sheet.contains(c.arrowImage));

        // Область набора: плоская (без скруглений) и без :focus-
        // обводки — визуального шума при наборе нет; фон/текст заданы.
        const QString editorRule =
            firstRule(sheet, QStringLiteral("QPlainTextEdit#mainEditor"));
        QVERIFY2(!editorRule.isEmpty(), "Правило редактора не найдено");
        QVERIFY(editorRule.contains(QStringLiteral("background-color:")));
        QVERIFY(editorRule.contains(QStringLiteral("border: none")));
        QVERIFY(!editorRule.contains(QStringLiteral("border-radius")));
        QVERIFY(!sheet.contains(QStringLiteral("#mainEditor:focus")));

        // Индикатор состояния и CTA на месте.
        QVERIFY(sheet.contains(QStringLiteral("QLabel#stateIndicator")));
        QVERIFY(
            sheet.contains(QStringLiteral("QPushButton#generateButton")));
    }
}

void ThemeTest::stateColorsDistinguishErrorAndGenerating()
{
    using State = SuggestionController::State;

    for (const Theme::Scheme scheme :
         {Theme::Scheme::Ptuch, Theme::Scheme::Light}) {
        const Theme::Colors c = Theme::colors(scheme);
        const QColor error =
            Theme::stateColor(scheme, State::Error);
        const QColor generating =
            Theme::stateColor(scheme, State::Generating);
        const QColor waiting =
            Theme::stateColor(scheme, State::Debouncing);
        const QColor ready = Theme::stateColor(scheme, State::Idle);

        // Ошибка — красный, генерация — синий, ожидание — янтарный,
        // готов — зелёный; цвета различимы и читаются на фоне схемы.
        QVERIFY(error.red() > error.green() && error.red() > error.blue());
        QVERIFY(sameColor(generating, c.secondary));
        QVERIFY(!sameColor(generating, error));
        QVERIFY(!sameColor(waiting, generating));
        QVERIFY(!sameColor(waiting, error));
        QVERIFY(!sameColor(ready, error));
        QVERIFY(!sameColor(ready, generating));
        QVERIFY(contrast(error, c.window) >= 4.5);
        QVERIFY(contrast(generating, c.window) >= 4.5);
        QVERIFY(contrast(waiting, c.window) >= 4.5);
        QVERIFY(contrast(ready, c.window) >= 4.5);
    }

    // Заголовки индикатора одинаковы для всех схем и различимы
    // по состояниям.
    QCOMPARE(Theme::stateTitle(State::Error), QStringLiteral("Error"));
    QCOMPARE(Theme::stateTitle(State::Generating),
             QStringLiteral("Generating"));
    QVERIFY(Theme::stateTitle(State::Debouncing)
            != Theme::stateTitle(State::Ready));
    QVERIFY(Theme::stateTitle(State::Debouncing)
            != Theme::stateTitle(State::Error));
}

void ThemeTest::systemSchemeOmitsAllStyling()
{
    // default: никаких stylesheet'ов — контролы платформенные.
    QVERIFY(Theme::mainStyleSheet(Theme::Scheme::System).isEmpty());
    QVERIFY(Theme::panelStyleSheet(Theme::Scheme::System).isEmpty());
    QVERIFY(Theme::dialogStyleSheet(Theme::Scheme::System).isEmpty());

    // Маркер профиля — цвет данных остаётся, декора нет: без радиуса,
    // нейтральная рамка.
    const QColor profileColor(QStringLiteral("#123456"));
    const QString systemMarker =
        Theme::colorMarkerStyle(profileColor, Theme::Scheme::System);
    QVERIFY(systemMarker.contains(profileColor.name()));
    QVERIFY(!systemMarker.contains(QStringLiteral("border-radius")));

    // Темизированные схемы — декор (скругления и градиент) есть.
    for (const Theme::Scheme scheme :
         {Theme::Scheme::Ptuch, Theme::Scheme::Light}) {
        const QString main = Theme::mainStyleSheet(scheme);
        QVERIFY(main.contains(QStringLiteral("border-radius")));
        QVERIFY(main.contains(QStringLiteral("qlineargradient")));
        QVERIFY(Theme::panelStyleSheet(scheme)
                    .contains(QStringLiteral("border-radius")));
        QVERIFY(Theme::dialogStyleSheet(scheme)
                    .contains(QStringLiteral("border-radius")));
        QVERIFY(Theme::colorMarkerStyle(profileColor, scheme)
                    .contains(QStringLiteral("border-radius")));
        QVERIFY(Theme::colorMarkerStyle(profileColor, scheme)
                    .contains(profileColor.name()));
        // Состояния и цвета в плоском сравнении не зависят от
        // схемного «выключения» — System просто пуст, остальное есть.
        QVERIFY(main.contains(QStringLiteral(":focus")));
        QVERIFY(main.contains(Theme::colors(scheme).base.name()));
    }
}

void ThemeTest::panelAndDialogSheetsShareThemeColors()
{
    for (const Theme::Scheme scheme :
         {Theme::Scheme::Ptuch, Theme::Scheme::Light}) {
        const Theme::Colors c = Theme::colors(scheme);

        const QString panel = Theme::panelStyleSheet(scheme);
        QVERIFY(!panel.isEmpty());
        QVERIFY2(hasNoLeftoverTokens(panel),
                 "Stylesheet панели содержит незамещённые токены");
        // Те же цвета, что у остальной темы (а не своя палитра).
        QVERIFY(panel.contains(c.panel.name()));
        QVERIFY(panel.contains(c.labelColor.name()));
        QVERIFY(panel.contains(c.secondary.name()));
        QVERIFY(panel.contains(c.accentInk.name())); // маркер суммы
        // Слои панели и её состояния.
        QVERIFY(panel.contains(QStringLiteral("QSlider::groove")));
        QVERIFY(panel.contains(QStringLiteral("QCheckBox::indicator")));
        QVERIFY(panel.contains(QStringLiteral(":disabled")));
        QVERIFY(panel.contains(QStringLiteral(":hover")));
        QVERIFY(panel.contains(QStringLiteral(":focus")));

        const QString dialog = Theme::dialogStyleSheet(scheme);
        QVERIFY(!dialog.isEmpty());
        QVERIFY2(hasNoLeftoverTokens(dialog),
                 "Stylesheet диалога содержит незамещённые токены");
        QVERIFY(dialog.contains(c.panel.name()));
        QVERIFY(dialog.contains(QStringLiteral(":hover")));
        QVERIFY(dialog.contains(QStringLiteral(":pressed")));
        QVERIFY(dialog.contains(QStringLiteral(":disabled")));
        QVERIFY(dialog.contains(QStringLiteral(":focus")));

        // Маркер профиля несёт его цвет + рамку схемы.
        const QColor profileColor(QStringLiteral("#123456"));
        const QString marker =
            Theme::colorMarkerStyle(profileColor, scheme);
        QVERIFY(marker.contains(profileColor.name()));
        QVERIFY(marker.contains(c.border.name()));
    }
}

QTEST_GUILESS_MAIN(ThemeTest)

#include "theme_test.moc"
