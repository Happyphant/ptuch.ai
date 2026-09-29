// style_panel_test.cpp
#include <QtTest>

#include "UI/style_panel.h"
#include "suggestion/style_mixer.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>

// ---------------------------------------------------------------------------
// StylePanel: строки каждого профиля (маркер, название, слайдер 0..100,
// число, нормализованная доля, чекбокс, сброс), единый сигнал
// styleMixChanged (ровно один на действие, без циклических обновлений),
// сумма и нормализованные значения из модели, Reset Mix, тёмная тема.
// ---------------------------------------------------------------------------
class StylePanelTest final : public QObject
{
    Q_OBJECT

private slots:
    void rowsCreatedForEveryProfile();
    void sliderChangeEmitsSingleSignalAndUpdatesModel();
    void normalizedValuesAndSumAreShown();
    void toggleExcludesProfileAndDisablesSlider();
    void perStyleResetZeroesWeightWithSingleSignal();
    void resetMixZeroesWeightsWithSingleSignal();
    void panelIsDarkThemeReady();

private:
    // Счётчик эмиссий styleMixChanged + последний payload (то, что
    // уйдёт в контроллер). Ловится connect'ом с лямбдой.
    struct Capture {
        int count = 0;
        QVector<StyleWeight> last;
    };
    Capture m_capture;

    // Подключить счётчик к сигналу панели и обнулить его.
    void watch(StylePanel& panel)
    {
        m_capture = Capture();
        connect(&panel, &StylePanel::styleMixChanged, this,
                [this](const QVector<StyleWeight>& weights) {
                    ++m_capture.count;
                    m_capture.last = weights;
                });
    }

    static QSlider* slider(StylePanel& panel, const QString& id)
    {
        return panel.findChild<QSlider*>(
            QStringLiteral("slider_%1").arg(id));
    }

    static QLabel* label(StylePanel& panel, const QString& objectName)
    {
        return panel.findChild<QLabel*>(objectName);
    }

    static double payloadSum(const QVector<StyleWeight>& weights)
    {
        double sum = 0.0;
        for (const StyleWeight& weight : weights)
            sum += weight.weight;
        return sum;
    }
};

// Каждый встроенный профиль получает полную строку управления; стартовое
// состояние — все веса 0, профили включены, активных нет; построение
// панели не испускает сигналов.
void StylePanelTest::rowsCreatedForEveryProfile()
{
    StylePanel panel;
    QCOMPARE(panel.objectName(), QStringLiteral("stylePanel"));
    watch(panel);

    const QVector<StyleProfile> profiles = builtinStyleProfiles();
    QVERIFY(!profiles.isEmpty());

    for (const StyleProfile& profile : profiles) {
        const QString id = profile.id;
        auto* marker = panel.findChild<QLabel*>(
            QStringLiteral("colorMarker_%1").arg(id));
        auto* name = panel.findChild<QLabel*>(
            QStringLiteral("nameLabel_%1").arg(id));
        auto* value = panel.findChild<QLabel*>(
            QStringLiteral("valueLabel_%1").arg(id));
        auto* norm = panel.findChild<QLabel*>(
            QStringLiteral("normalizedLabel_%1").arg(id));
        auto* check = panel.findChild<QCheckBox*>(
            QStringLiteral("enabledCheck_%1").arg(id));
        auto* reset = panel.findChild<QPushButton*>(
            QStringLiteral("resetButton_%1").arg(id));

        QVERIFY2(marker && name && value && norm && check && reset,
                 qPrintable(QStringLiteral("Строка профиля «%1» "
                                           "неполная").arg(profile.displayName)));
        QVERIFY2(slider(panel, id), "Слайдер отсутствует");

        // Название и цветовой маркер — из модели профилей.
        QCOMPARE(name->text(), profile.displayName);
        QVERIFY2(marker->styleSheet().contains(profile.color.name()),
                 "Маркер не покрашен в цвет профиля");

        // Слайдер строго 0..100, стартовое состояние.
        QCOMPARE(slider(panel, id)->minimum(), 0);
        QCOMPARE(slider(panel, id)->maximum(), 100);
        QCOMPARE(slider(panel, id)->value(), 0);
        QCOMPARE(value->text(), QStringLiteral("0"));
        QCOMPARE(norm->text(),
                 QStringLiteral("—")); // активных нет — доли нет
        QVERIFY(check->isChecked());
        QVERIFY(slider(panel, id)->isEnabled());
    }

    // Сумма и Reset Mix — внизу панели.
    QVERIFY(label(panel, QStringLiteral("sumLabel")));
    QVERIFY2(panel.findChild<QPushButton*>(
                 QStringLiteral("resetMixButton")),
             "Кнопка Reset Mix отсутствует");
    QCOMPARE(label(panel, QStringLiteral("sumLabel"))->text(),
             QStringLiteral("Сумма: 0.00"));

    // Ни построение, ни обращения к виджетам ничего не изменили.
    QCOMPARE(m_capture.count, 0);
}

// Изменение слайдера: ровно один сигнал, модель обновлена, подписи
// освежены; повтор/отсутствие изменений — без сигнала (нет петель).
void StylePanelTest::sliderChangeEmitsSingleSignalAndUpdatesModel()
{
    StylePanel panel;
    watch(panel);

    QSlider* pushkinSlider = slider(panel, QStringLiteral("pushkin"));
    QVERIFY(pushkinSlider);
    pushkinSlider->setValue(60);

    // Ровно один сигнал: обновление подписей не порождает новых.
    QCOMPARE(m_capture.count, 1);
    QCOMPARE(m_capture.last.size(), 1);
    QCOMPARE(m_capture.last.first().styleId, QStringLiteral("pushkin"));
    QVERIFY(qFuzzyCompare(double(m_capture.last.first().weight), 1.0));

    // Модель отделена от UI: состояние изменено в StyleMixer, а не в
    // каком-то виджетном «хранилище».
    QVERIFY(qFuzzyCompare(
        panel.mixer().profile(QStringLiteral("pushkin"))->weight, 0.6));
    QCOMPARE(panel.mix().size(), 1);

    // Подписи строки и суммы соответствуют.
    QCOMPARE(label(panel, QStringLiteral("valueLabel_pushkin"))->text(),
             QStringLiteral("60"));
    QCOMPARE(label(panel, QStringLiteral("normalizedLabel_pushkin"))->text(),
             QStringLiteral("1.00"));
    QCOMPARE(label(panel, QStringLiteral("sumLabel"))->text(),
             QStringLiteral("Сумма: 0.60"));

    // То же значение — изменения нет, сигнала нет.
    pushkinSlider->setValue(60);
    QCOMPARE(m_capture.count, 1);

    // Каждое реальное изменение — ровно один сигнал (нет циклов).
    pushkinSlider->setValue(30);
    pushkinSlider->setValue(90);
    QCOMPARE(m_capture.count, 3);
}

// Сумма активных весов и нормализованные доли считаются моделью и
// показываются корректно (нормализация на лету, включая «нештатный» итог).
void StylePanelTest::normalizedValuesAndSumAreShown()
{
    StylePanel panel;
    watch(panel);

    slider(panel, QStringLiteral("pushkin"))->setValue(60);
    slider(panel, QStringLiteral("official"))->setValue(40);

    // 0.60 + 0.40 = 1.00 -> доли 0.60/0.40; остальные — «—».
    QCOMPARE(label(panel, QStringLiteral("sumLabel"))->text(),
             QStringLiteral("Сумма: 1.00"));
    QCOMPARE(
        label(panel, QStringLiteral("normalizedLabel_pushkin"))->text(),
        QStringLiteral("0.60"));
    QCOMPARE(
        label(panel, QStringLiteral("normalizedLabel_official"))->text(),
        QStringLiteral("0.40"));
    QCOMPARE(
        label(panel, QStringLiteral("normalizedLabel_tyutchev"))->text(),
        QStringLiteral("—"));
    QCOMPARE(m_capture.count, 2);

    // Итог не равен 1.00: 0.80 + 0.40 = 1.20 -> доли 0.67/0.33.
    slider(panel, QStringLiteral("pushkin"))->setValue(80);
    QCOMPARE(label(panel, QStringLiteral("sumLabel"))->text(),
             QStringLiteral("Сумма: 1.20"));
    QCOMPARE(
        label(panel, QStringLiteral("normalizedLabel_pushkin"))->text(),
        QStringLiteral("0.67"));
    QCOMPARE(
        label(panel, QStringLiteral("normalizedLabel_official"))->text(),
        QStringLiteral("0.33"));
    QCOMPARE(m_capture.count, 3);

    // Payload сигнала — нормализованные веса (float): сумма долей
    // равна 1.0 с точностью float-представления.
    QVERIFY2(qAbs(payloadSum(m_capture.last) - 1.0) < 1e-5,
             "Сумма нормализованных весов должна быть ровно 1.0 (±float)");
}

// Чекбокс: выключенный профиль уходит из микса (и из суммы, и из
// долей), его слайдер гаснет; включение возвращает его без эха.
void StylePanelTest::toggleExcludesProfileAndDisablesSlider()
{
    StylePanel panel;
    watch(panel);

    QSlider* pushkinSlider = slider(panel, QStringLiteral("pushkin"));
    pushkinSlider->setValue(50);
    QCOMPARE(m_capture.count, 1);

    auto* check = panel.findChild<QCheckBox*>(
        QStringLiteral("enabledCheck_pushkin"));
    QVERIFY(check);
    check->setChecked(false);

    // Тоггл — один сигнал (setEnabled слайдера не эмитит ничего).
    QCOMPARE(m_capture.count, 2);
    QVERIFY(m_capture.last.isEmpty()); // активных не осталось
    QVERIFY2(!pushkinSlider->isEnabled(),
             "Слайдер выключенного профиля должен погаснуть");
    QCOMPARE(
        label(panel, QStringLiteral("normalizedLabel_pushkin"))->text(),
        QStringLiteral("—"));
    QCOMPARE(label(panel, QStringLiteral("sumLabel"))->text(),
             QStringLiteral("Сумма: 0.00"));

    // Включение возвращает профиль в микс.
    check->setChecked(true);
    QCOMPARE(m_capture.count, 3);
    QCOMPARE(m_capture.last.size(), 1);
    QVERIFY(pushkinSlider->isEnabled());
    QCOMPARE(
        label(panel, QStringLiteral("normalizedLabel_pushkin"))->text(),
        QStringLiteral("1.00"));
}

// Сброс одного профиля: слайдер в 0, один сигнал; повторный сброс при
// нулевом весе — без изменений и без сигнала.
void StylePanelTest::perStyleResetZeroesWeightWithSingleSignal()
{
    StylePanel panel;
    watch(panel);

    QSlider* pushkinSlider = slider(panel, QStringLiteral("pushkin"));
    pushkinSlider->setValue(70);
    QCOMPARE(m_capture.count, 1);

    auto* reset = panel.findChild<QPushButton*>(
        QStringLiteral("resetButton_pushkin"));
    QVERIFY(reset);
    reset->click();

    // Ровно один сигнал на клик (setValue -> valueChanged -> путь).
    QCOMPARE(m_capture.count, 2);
    QCOMPARE(pushkinSlider->value(), 0);
    QCOMPARE(label(panel, QStringLiteral("valueLabel_pushkin"))->text(),
             QStringLiteral("0"));
    QVERIFY(m_capture.last.isEmpty());

    // Уже нулевой вес — сбрасывать нечего: сигнала нет.
    reset->click();
    QCOMPARE(m_capture.count, 2);
}

// Reset Mix: обнуляет ВСЕ веса одним жестом и ровно одним сигналом
// (программные setValue гасятся QSignalBlocker'ом).
void StylePanelTest::resetMixZeroesWeightsWithSingleSignal()
{
    StylePanel panel;
    watch(panel);

    slider(panel, QStringLiteral("pushkin"))->setValue(60);
    slider(panel, QStringLiteral("tyutchev"))->setValue(30);
    QCOMPARE(m_capture.count, 2);
    QCOMPARE(m_capture.last.size(), 2);

    auto* resetMix = panel.findChild<QPushButton*>(
        QStringLiteral("resetMixButton"));
    QVERIFY(resetMix);
    resetMix->click();

    // Один Reset Mix — ровно один сигнал.
    QCOMPARE(m_capture.count, 3);
    QVERIFY(m_capture.last.isEmpty());

    for (const StyleProfile& profile : builtinStyleProfiles()) {
        QCOMPARE(slider(panel, profile.id)->value(), 0);
        QCOMPARE(label(panel,
                       QStringLiteral("valueLabel_%1").arg(profile.id))
                     ->text(),
                 QStringLiteral("0"));
        QCOMPARE(label(panel,
                       QStringLiteral("normalizedLabel_%1").arg(profile.id))
                     ->text(),
                 QStringLiteral("—"));
    }
    QCOMPARE(label(panel, QStringLiteral("sumLabel"))->text(),
             QStringLiteral("Сумма: 0.00"));

    // Модель согласована с виджетами.
    QVERIFY(panel.mixer().activeProfiles().isEmpty());

    // Пустой микс: повторный клик ничего не меняет.
    resetMix->click();
    QCOMPARE(m_capture.count, 3);
}

// Панель приносит собственный тёмный stylesheet (фон, текст, акценты).
void StylePanelTest::panelIsDarkThemeReady()
{
    StylePanel panel;

    const QString sheet = panel.styleSheet();
    QVERIFY2(!sheet.isEmpty(),
             "Панель без stylesheet не готова к тёмной теме");
    QVERIFY(sheet.contains(
        QStringLiteral("#3a3a3a"))); // тёмный фон панели
    QVERIFY(sheet.contains(
        QStringLiteral("#dddddd"))); // светлый текст подписей
    QVERIFY(sheet.contains(
        QStringLiteral("#42a5f5"))); // акцент слайдера и чекбокса
    QVERIFY(sheet.contains(QStringLiteral("QSlider::groove")));
    QVERIFY(sheet.contains(QStringLiteral("QCheckBox::indicator")));
}

QTEST_MAIN(StylePanelTest)

#include "style_panel_test.moc"
