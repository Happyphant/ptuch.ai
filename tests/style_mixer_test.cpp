// style_mixer_test.cpp
#include <QtTest>

#include "suggestion/style_mixer.h"
#include "suggestion/style_profile.h"

#include <QStringList>
#include <QVector>

#include <limits>
#include <utility>

// ---------------------------------------------------------------------------
// Стилевые профили: встроенные наборы (обязательные поля, юридически
// нейтральные имена), StyleMixer — кламп и санитизация весов,
// предсказуемая сумма активных весов (нормализация до 1.0, в т.ч. на
// лету), исключение нулевых/выключенных профилей из prompt'а и
// компактность инструкции (не разрастается от числа событий UI).
//
// QTEST_GUILESS_MAIN и отсутствие Qt6::Widgets в линковке — проверка
// того, что StyleMixer не зависит от Qt-виджетов (только Core/Gui:
// QColor — цвет профиля, на генерацию не влияет).
// ---------------------------------------------------------------------------
class StyleMixerTest final : public QObject
{
    Q_OBJECT

private slots:
    void builtinProfilesExposeRequiredFields();
    void builtinProfilesDefaultToInactive();

    void mixerSanitizesWeightOnConstruction();
    void setWeightClampsAndRejectsUnknownId();
    void setEnabledFlipsParticipation();

    void zeroAndDisabledProfilesStayOutOfPrompt();
    void normalizeMakesActiveSumExactlyOne();
    void normalizeKeepsInactiveUntouched();
    void styleWeightsAreNormalizedOnTheFly();

    void instructionIsCompactPerActiveProfile();
    void instructionExcludesProfileWithoutText();
    void instructionDoesNotGrowWithRepeatedCalls();

    // Опциональные поля LoRA-адаптеров (данные под будущий
    // adapter-capable backend; MVP их не использует).
    void adapterFieldsDefaultToNoAdapter();
    void adapterCompatibilityErrors();
    void findActiveAdapterPicksActiveProfile();
};

namespace {

bool containsDisplayName(const QVector<StyleProfile>& profiles,
                         const QString& name)
{
    for (const StyleProfile& profile : profiles) {
        if (profile.displayName == name)
            return true;
    }
    return false;
}

double weightsSum(const QVector<StyleWeight>& weights)
{
    double sum = 0.0;
    for (const StyleWeight& weight : weights)
        sum += weight.weight;
    return sum;
}

} // namespace

// Все4 встроенных профиля: обязательные поля непусты, id уникальны,
// имена — исторические авторы либо нейтральные описательные (ТЗ:
// живые авторы/публичные персоны — только нейтральные названия).
void StyleMixerTest::builtinProfilesExposeRequiredFields()
{
    const QVector<StyleProfile> profiles = builtinStyleProfiles();
    QCOMPARE(profiles.size(), 4);

    QStringList ids;
    for (const StyleProfile& profile : profiles) {
        QVERIFY2(!profile.id.isEmpty(), "id обязателен");
        QVERIFY2(!profile.displayName.isEmpty(), "displayName обязателен");
        QVERIFY2(!profile.description.isEmpty(), "description обязателен");
        QVERIFY2(!profile.promptInstruction.isEmpty(),
                 "promptInstruction обязателен");
        QVERIFY2(profile.color.isValid(), "цвет индикатора обязателен");
        QVERIFY2(profile.weight == 0.0,
                 "встроенные профили стартуют с нулевым весом");
        QVERIFY2(profile.enabled, "встроенные профили включены");
        QVERIFY2(!ids.contains(profile.id), "id должен быть уникален");
        ids << profile.id;
    }

    QVERIFY(containsDisplayName(profiles, QStringLiteral("Пушкин")));
    QVERIFY(containsDisplayName(profiles, QStringLiteral("Тютчев")));
    QVERIFY(containsDisplayName(profiles, QStringLiteral("Киберпанк 80-х")));
    QVERIFY(containsDisplayName(profiles,
                                QStringLiteral("Официальный стиль")));
}

// Свежий микшер без выставленных весов не даёт ни активных профилей,
// ни prompt-инструкции, ни весов для запроса.
void StyleMixerTest::builtinProfilesDefaultToInactive()
{
    const StyleMixer mixer;
    QCOMPARE(mixer.profiles().size(), 4);
    QVERIFY(mixer.activeProfiles().isEmpty());
    QVERIFY(mixer.buildInstruction().isEmpty());
    QVERIFY(mixer.styleWeights().isEmpty());
}

// Санитизация веса в конструкторе: мусор из QSettings/внешнего кода
// (NaN, >1, отрицательный) не доходит до нормализации.
void StyleMixerTest::mixerSanitizesWeightOnConstruction()
{
    QVector<StyleProfile> custom = builtinStyleProfiles();
    custom[0].weight = 5.0;
    custom[1].weight = -2.0;
    custom[2].weight = std::numeric_limits<double>::quiet_NaN();

    const StyleMixer mixer(std::move(custom));
    QCOMPARE(mixer.profiles().at(0).weight, 1.0);
    QCOMPARE(mixer.profiles().at(1).weight, 0.0);
    QCOMPARE(mixer.profiles().at(2).weight, 0.0);
}

// setWeight: кламп в [0; 1], неизвестный id — false (и ничего не меняет).
void StyleMixerTest::setWeightClampsAndRejectsUnknownId()
{
    StyleMixer mixer;

    QVERIFY(mixer.setWeight(QStringLiteral("pushkin"), 5.0));
    QCOMPARE(mixer.profile(QStringLiteral("pushkin"))->weight, 1.0);

    QVERIFY(mixer.setWeight(QStringLiteral("pushkin"), -1.0));
    QCOMPARE(mixer.profile(QStringLiteral("pushkin"))->weight, 0.0);

    QVERIFY(mixer.setWeight(QStringLiteral("tyutchev"), 0.25));
    QCOMPARE(mixer.profile(QStringLiteral("tyutchev"))->weight, 0.25);

    QVERIFY(!mixer.setWeight(QStringLiteral("no-such-id"), 1.0));
    QVERIFY(!mixer.setEnabled(QStringLiteral("no-such-id"), false));
    QVERIFY(mixer.profile(QStringLiteral("no-such-id")) == nullptr);
}

// Выключенный профиль с ненулевым весом не участвует и потом снова
// участвует, когда его включают обратно.
void StyleMixerTest::setEnabledFlipsParticipation()
{
    StyleMixer mixer;
    mixer.setWeight(QStringLiteral("official"), 0.7);

    QVERIFY(mixer.setEnabled(QStringLiteral("official"), false));
    QVERIFY(mixer.activeProfiles().isEmpty());
    QVERIFY(mixer.styleWeights().isEmpty());
    QVERIFY(mixer.buildInstruction().isEmpty());

    QVERIFY(mixer.setEnabled(QStringLiteral("official"), true));
    QCOMPARE(mixer.activeProfiles().size(), 1);
    QCOMPARE(mixer.styleWeights().size(), 1);
}

// Правило «ноль в prompt не попадает»: активны только профили с
// enabled && weight > 0 — и в весах, и в инструкции.
void StyleMixerTest::zeroAndDisabledProfilesStayOutOfPrompt()
{
    StyleMixer mixer;
    QVERIFY(mixer.setWeight(QStringLiteral("pushkin"), 0.5));
    QVERIFY(mixer.setWeight(QStringLiteral("official"), 0.5));
    // Остальные (Тютчев, Киберпанк) остаются с весом 0.

    QString instruction = mixer.buildInstruction();
    QCOMPARE(instruction.split(QLatin1Char('\n')).size(), 2);
    QVERIFY(instruction.contains(QStringLiteral("Пушкин")));
    QVERIFY(instruction.contains(QStringLiteral("Официальный стиль")));
    QVERIFY(!instruction.contains(QStringLiteral("Тютчев")));
    QVERIFY(!instruction.contains(QStringLiteral("Киберпанк")));
    QCOMPARE(mixer.styleWeights().size(), 2);

    // Нулевой профиль уходит из prompt'а.
    QVERIFY(mixer.setWeight(QStringLiteral("pushkin"), 0.0));
    instruction = mixer.buildInstruction();
    QVERIFY(!instruction.contains(QStringLiteral("Пушкин")));
    QVERIFY(instruction.contains(QStringLiteral("Официальный стиль")));
    QCOMPARE(mixer.styleWeights().size(), 1);
    QCOMPARE(mixer.styleWeights().first().styleId,
             QStringLiteral("official"));
}

// Правило «сумма активных весов предсказуема»: normalize() приводит
// активные веса к сумме ровно 1.0, сохраняя пропорции, и идемпотентен.
void StyleMixerTest::normalizeMakesActiveSumExactlyOne()
{
    StyleMixer mixer;
    mixer.setWeight(QStringLiteral("pushkin"), 0.8);
    mixer.setWeight(QStringLiteral("tyutchev"), 0.4);
    // Всего 1.2 -> после нормализации 2/3 и 1/3.

    mixer.normalize();

    const double pushkin = mixer.profile(QStringLiteral("pushkin"))->weight;
    const double tyutchev = mixer.profile(QStringLiteral("tyutchev"))->weight;
    QVERIFY2(qFuzzyCompare(pushkin + tyutchev, 1.0),
             "Сумма активных весов должна быть ровно 1.0");
    QVERIFY2(qFuzzyCompare(pushkin, tyutchev * 2.0),
             "Пропорции 0.8:0.4 = 2:1 должны сохраниться");

    // Идемпотентность: повторная нормализация не меняет (сумма уже 1.0).
    mixer.normalize();
    QVERIFY(qFuzzyCompare(mixer.profile(QStringLiteral("pushkin"))->weight,
                          pushkin));
}

// normalize() не трогает неактивные профили (нулевые и выключенные) и
// безопасен при пустом активном наборе.
void StyleMixerTest::normalizeKeepsInactiveUntouched()
{
    StyleMixer mixer;
    mixer.setWeight(QStringLiteral("pushkin"), 0.5);
    mixer.setWeight(QStringLiteral("official"), 0.0); // неактивен
    mixer.setEnabled(QStringLiteral("cyberpunk80"), false);
    mixer.setWeight(QStringLiteral("cyberpunk80"), 1.0); // выключен

    mixer.normalize();

    QCOMPARE(mixer.profile(QStringLiteral("official"))->weight, 0.0);
    QCOMPARE(mixer.profile(QStringLiteral("cyberpunk80"))->weight, 1.0);
    QCOMPARE(mixer.profile(QStringLiteral("tyutchev"))->weight, 0.0);

    // Активных нет — normalize() не меняет ничего и не падает.
    StyleMixer empty;
    empty.normalize();
    QVERIFY(empty.activeProfiles().isEmpty());
    QVERIFY(empty.styleWeights().isEmpty());
}

// styleWeights() нормализует на лету: вызывать normalize() заранее не
// нужно, сумма всегда 1.0, неактивные не попадают в вектор.
void StyleMixerTest::styleWeightsAreNormalizedOnTheFly()
{
    StyleMixer mixer;
    mixer.setWeight(QStringLiteral("pushkin"), 0.9);
    mixer.setWeight(QStringLiteral("official"), 0.3);

    const QVector<StyleWeight> weights = mixer.styleWeights();
    QCOMPARE(weights.size(), 2);
    QCOMPARE(weights.at(0).styleId, QStringLiteral("pushkin"));
    QCOMPARE(weights.at(1).styleId, QStringLiteral("official"));
    QVERIFY2(qFuzzyCompare(weightsSum(weights), 1.0),
             "Сумма нормализованных весов = 1.0 без вызова normalize()");
    // Пропорции 0.9:0.3 = 3:1.
    QVERIFY(qFuzzyCompare(double(weights.at(0).weight),
                          double(weights.at(1).weight) * 3.0));
}

// Компактность: по одной строке на активный профиль, нормализованные
// веса в скобках, неактивные не упоминаются.
void StyleMixerTest::instructionIsCompactPerActiveProfile()
{
    StyleMixer mixer;
    mixer.setWeight(QStringLiteral("pushkin"), 0.5);
    mixer.setWeight(QStringLiteral("official"), 0.5);

    const QString instruction = mixer.buildInstruction();
    const QStringList lines = instruction.split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 2);
    QVERIFY(instruction.contains(QStringLiteral("Пушкин (0.50):")));
    QVERIFY(instruction.contains(QStringLiteral("Официальный стиль (0.50):")));

    // Асимметричные веса показываются нормализованными (0.8/0.4 -> 2:1).
    mixer.setWeight(QStringLiteral("pushkin"), 0.8);
    mixer.setWeight(QStringLiteral("official"), 0.4);
    const QString asym = mixer.buildInstruction();
    QVERIFY(asym.contains(QStringLiteral("Пушкин (0.67):")));
    QVERIFY(asym.contains(QStringLiteral("Официальный стиль (0.33):")));
}

// Профиль с пустым promptInstruction не даёт пустых строк в prompt'е.
void StyleMixerTest::instructionExcludesProfileWithoutText()
{
    QVector<StyleProfile> custom = builtinStyleProfiles();
    custom[0].promptInstruction.clear(); // pushkin

    StyleMixer mixer(std::move(custom));
    mixer.setWeight(QStringLiteral("pushkin"), 1.0);
    mixer.setWeight(QStringLiteral("official"), 0.5);

    const QString instruction = mixer.buildInstruction();
    QVERIFY(!instruction.contains(QStringLiteral("Пушкин (")));
    QVERIFY(instruction.contains(QStringLiteral("Официальный стиль")));
    QCOMPARE(instruction.split(QLatin1Char('\n')).size(), 1);
}

// Правило «prompt не разрастается пропорционально числу событий UI»:
// buildInstruction() — чистая функция состояния, 100 вызовов дают тот же
// результат; смена веса перестраивает строку, а не наращивает её.
void StyleMixerTest::instructionDoesNotGrowWithRepeatedCalls()
{
    StyleMixer mixer;
    mixer.setWeight(QStringLiteral("pushkin"), 0.6);
    mixer.setWeight(QStringLiteral("tyutchev"), 0.3);

    const QString first = mixer.buildInstruction();
    QVERIFY(!first.isEmpty());

    for (int i = 0; i < 100; ++i) {
        const QString again = mixer.buildInstruction();
        QCOMPARE(again, first);
        QCOMPARE(again.size(), first.size());
    }

    // «Событие UI» (смена веса) пересчитывает, а не добавляет к прежней.
    mixer.setWeight(QStringLiteral("pushkin"), 0.1);
    const QString changed = mixer.buildInstruction();
    QVERIFY(changed != first);
    QVERIFY(changed.contains(QStringLiteral("0.25"))); // 0.1 / (0.1+0.3)
    QVERIFY(changed.contains(QStringLiteral("0.75"))); // 0.3 / (0.1+0.3)
}

// Опциональные поля адаптеров: у встроенных профилей они пусты —
// текущий GGUF-бэкенд работает без изменений; hasAdapter зависит
// только от непустого adapterPath, adapterScale по умолчанию 1.0.
void StyleMixerTest::adapterFieldsDefaultToNoAdapter()
{
    const QVector<StyleProfile> profiles = builtinStyleProfiles();
    QCOMPARE(profiles.size(), 4);

    for (const StyleProfile& profile : profiles) {
        QVERIFY2(!hasAdapter(profile), qPrintable(profile.id));
        QVERIFY(profile.adapterPath.isEmpty());
        QVERIFY(profile.adapterType.isEmpty());
        QVERIFY(profile.baseModelId.isEmpty());
        QVERIFY(profile.promptTag.isEmpty());
        QCOMPARE(profile.adapterScale, 1.0);
    }

    // Прочие поля без adapterPath адаптера не объявляют.
    StyleProfile typeOnly;
    typeOnly.adapterType = QStringLiteral("lora");
    typeOnly.baseModelId = QStringLiteral("qwen2.5-3b");
    QVERIFY(!hasAdapter(typeOnly));

    // Признак «адаптер объявлен» — ровно непустой путь.
    StyleProfile withPath;
    withPath.adapterPath = QStringLiteral("/models/x-lora.safetensors");
    QVERIFY(hasAdapter(withPath));
}

// Совместимость адаптера с базовой моделью backend'а: строгие ошибки
// вместо тихого «пропустить» — несовместимость видна ДО инференса.
void StyleMixerTest::adapterCompatibilityErrors()
{
    StyleProfile profile;
    profile.adapterPath = QStringLiteral("/models/a-lora.safetensors");

    // Адаптера нет — проверять нечего (backend работает без адаптеров),
    // даже если базовая модель backend'а известна.
    StyleProfile withoutAdapter;
    QCOMPARE(
        checkAdapterCompatibility(withoutAdapter,
                                  QStringLiteral("any-model")),
        QString());

    // У адаптера не указана базовая модель — «не проверено», ошибка.
    QString error =
        checkAdapterCompatibility(profile, QStringLiteral("other-model"));
    QVERIFY2(error.contains(QStringLiteral("baseModelId")),
             qPrintable(error));
    profile.baseModelId = QStringLiteral("qwen2.5-3b");

    // Базовая модель backend'а неизвестна — «не проверено», ошибка
    // (сам adapterPath в тексте не участвует — только идентификаторы).
    error = checkAdapterCompatibility(profile, QString());
    QVERIFY2(error.contains(QStringLiteral("неизвестна")), qPrintable(error));

    // Несовместимость: адаптер обучен под «qwen2.5-3b», загружена
    // другая модель — обе стороны видны в тексте ошибки.
    error = checkAdapterCompatibility(profile, QStringLiteral("other-7b"));
    QVERIFY2(error.contains(QStringLiteral("qwen2.5-3b")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("other-7b")), qPrintable(error));

    // Совпадение без учёта регистра — совместимо.
    QCOMPARE(checkAdapterCompatibility(profile, QStringLiteral("QWEN2.5-3B")),
             QString());
}

// Активный адаптер = непустой adapterPath + enabled + присутствие в
// нормализованных весах активных стилей с весом > 0; nullptr = нет.
void StyleMixerTest::findActiveAdapterPicksActiveProfile()
{
    StyleProfile adapter;
    adapter.id = QStringLiteral("styled");
    adapter.adapterPath = QStringLiteral("/models/styled-lora.safetensors");
    StyleProfile plain;
    plain.id = QStringLiteral("plain");

    const QVector<StyleProfile> profiles{adapter, plain};
    const QVector<StyleWeight> weights{
        StyleWeight{QStringLiteral("plain"), 0.4f},
        StyleWeight{QStringLiteral("styled"), 0.6f},
    };

    // Первый (и единственный) активный профиль с адаптером.
    const StyleProfile* found = findActiveAdapter(profiles, weights);
    QVERIFY(found != nullptr);
    QCOMPARE(found->id, QStringLiteral("styled"));

    // Профилю нет места в активных весах — активного адаптера нет.
    const QVector<StyleWeight> plainOnly{
        StyleWeight{QStringLiteral("plain"), 1.0f}};
    QVERIFY(findActiveAdapter(profiles, plainOnly) == nullptr);

    // Выключенный профиль не активен, даже если веса его содержат.
    StyleProfile disabled = adapter;
    disabled.enabled = false;
    const QVector<StyleProfile> disabledProfiles{disabled, plain};
    QVERIFY(findActiveAdapter(disabledProfiles, weights) == nullptr);

    // Нулевой вес — профиль не активен.
    const QVector<StyleWeight> zeroWeight{
        StyleWeight{QStringLiteral("styled"), 0.0f}};
    QVERIFY(findActiveAdapter(profiles, zeroWeight) == nullptr);

    // Без профилей — нечего искать.
    QVERIFY(findActiveAdapter(QVector<StyleProfile>(), weights) == nullptr);
}

QTEST_GUILESS_MAIN(StyleMixerTest)

#include "style_mixer_test.moc"
