// prompt_builder_test.cpp
#include <QtTest>

#include "settings/app_settings.h"
#include "suggestion/prompt_builder.h"
#include "suggestion/style_profile.h"

#include <QVector>

// ---------------------------------------------------------------------------
// PromptBuilder (режим автодополнения): system/user промпт + параметры
// генерации по контракту из prompt_builder.h —
//  1) system просит только продолжение и запрещает пояснения/кавычки/
//     метакомментарии; 2) контекст входит ровно один раз (user), дословно,
//     без декораций и без повтора в system; 3) язык документа сохраняется
//     (ru/en шаблоны + автоопределение); 4) смешанные стили блоком с
//     нормализованными весами; 5) детерминированная короткая структура;
//  6) параметры санируются диапазонами AppSettings.
//
// QTEST_GUILESS_MAIN + только Core/Gui в линковке: сборщик не зависит от
// виджетов и от QSettings (настройки приходят значениями).
// ---------------------------------------------------------------------------
class PromptBuilderTest final : public QObject
{
    Q_OBJECT

private slots:
    void noStylesProducesRulesOnlySystemPrompt();
    void zeroWeightStylesDoNotCountAsActive();
    void singleStyleAppendsInstructionBlock();
    void twoStylesAppearTogetherWithNormalizedWeights();

    void russianContextKeepsLanguage();
    void englishContextKeepsLanguage();
    void languageAutoDetectsFromContext();

    void emptyContextStillBuildsPrompt();

    void contextIsNeverDuplicatedOrDecorated();
    void structureIsDeterministic();
    void generationParamsAreSanitized();
};

namespace {

PromptInput makeRussianInput()
{
    PromptInput input;
    input.context = QStringLiteral(
        "Был прекрасный апрельский день, и в кабинете столоначальника");
    input.language = QStringLiteral("ru");
    return input;
}

PromptInput makeEnglishInput()
{
    PromptInput input;
    input.context = QStringLiteral(
        "The committee has reviewed the proposal and reached");
    input.language = QStringLiteral("en");
    return input;
}

// Встроенный профиль с выставленным весом (builtins стартуют с 0.0).
StyleProfile activeProfile(int index, double weight)
{
    StyleProfile profile = builtinStyleProfiles().at(index);
    profile.weight = weight;
    return profile;
}

} // namespace

// Отсутствие стилей: system — только фиксированные правила (короткие,
// без блока «Стили:»), user — контекст дословно. Правила прямо просят
// только продолжение и запрещают пояснения/кавычки/метакомментарии,
// контекст в system не дублируется.
void PromptBuilderTest::noStylesProducesRulesOnlySystemPrompt()
{
    PromptInput input = makeRussianInput();

    const AutocompletePrompt prompt = PromptBuilder::build(input);

    QVERIFY(!prompt.systemPrompt.isEmpty());
    QVERIFY(prompt.systemPrompt.startsWith(QStringLiteral("Автодополнение:")));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("только продолжение")));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("без пояснений")));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("метакомментариев")));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("Сохраняй язык")));
    // Блока стилей нет, и контекст в system не повторяется.
    QVERIFY(!prompt.systemPrompt.contains(QStringLiteral("Стили:")));
    QVERIFY(!prompt.systemPrompt.contains(input.context));
    // Короткий шаблон: правила без стилей укладываются в 400 символов.
    QVERIFY2(prompt.systemPrompt.size() < 400,
             "Промпт правил должен быть коротким");
    // user — контекст как есть.
    QCOMPARE(prompt.userPrompt, input.context);
}

// Нулевые/выключенные профили не считаются активными: «сырой» список со
// всеми builtins (вес 0) не даёт блока стилей.
void PromptBuilderTest::zeroWeightStylesDoNotCountAsActive()
{
    PromptInput input = makeRussianInput();
    input.styles = builtinStyleProfiles(); // веса 0.0

    const AutocompletePrompt prompt = PromptBuilder::build(input);

    QVERIFY(!prompt.systemPrompt.contains(QStringLiteral("Стили:")));
    QVERIFY(!prompt.systemPrompt.contains(QStringLiteral("Пушкин")));
    QVERIFY(prompt.systemPrompt.startsWith(QStringLiteral("Автодополнение:")));
}

// Один стиль: после правил — блок «Стили:» с именем, инструкцией и
// нормализованным весом (единственный активный -> 1.00).
void PromptBuilderTest::singleStyleAppendsInstructionBlock()
{
    PromptInput input = makeRussianInput();
    const StyleProfile official = activeProfile(3, 0.5); // Официальный стиль
    input.styles = QVector<StyleProfile>{official};

    const AutocompletePrompt prompt = PromptBuilder::build(input);

    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("Стили:")));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("Официальный стиль")));
    QVERIFY(prompt.systemPrompt.contains(official.promptInstruction));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("(1.00):")));
    // Структура детерминирована: сначала правила, потом стили.
    QVERIFY(prompt.systemPrompt.indexOf(QStringLiteral("Автодополнение:"))
            < prompt.systemPrompt.indexOf(QStringLiteral("Стили:")));
    QCOMPARE(prompt.userPrompt, input.context);
}

// Два стиля (смешанные): обе инструкции в system, веса нормализованы
// (0.8:0.4 -> 0.67:0.33), порядок — порядок входа; повторная сборка
// идентична байт в байт.
void PromptBuilderTest::twoStylesAppearTogetherWithNormalizedWeights()
{
    PromptInput input = makeRussianInput();
    input.styles = QVector<StyleProfile>{
        activeProfile(0, 0.8), // Пушкин
        activeProfile(3, 0.4), // Официальный стиль
    };

    const AutocompletePrompt prompt = PromptBuilder::build(input);

    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("Пушкин")));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("Официальный стиль")));
    QVERIFY(prompt.systemPrompt.contains(
        builtinStyleProfiles().at(0).promptInstruction));
    QVERIFY(prompt.systemPrompt.contains(
        builtinStyleProfiles().at(3).promptInstruction));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("Пушкин (0.67):")));
    QVERIFY(prompt.systemPrompt.contains(
        QStringLiteral("Официальный стиль (0.33):")));
    // Порядок входа сохраняется (детерминизм структуры).
    QVERIFY(prompt.systemPrompt.indexOf(QStringLiteral("Пушкин ("))
            < prompt.systemPrompt.indexOf(QStringLiteral("Официальный")));

    // Та же пара -> байт в байт тот же system prompt.
    const AutocompletePrompt again = PromptBuilder::build(input);
    QCOMPARE(again.systemPrompt, prompt.systemPrompt);
    QCOMPARE(again.userPrompt, prompt.userPrompt);
}

// Русский документ: русский шаблон правил, контекст в user — дословно,
// без кавычек и меток; в system текст документа отсутствует.
void PromptBuilderTest::russianContextKeepsLanguage()
{
    const PromptInput input = makeRussianInput();

    const AutocompletePrompt prompt = PromptBuilder::build(input);

    QVERIFY(prompt.systemPrompt.startsWith(QStringLiteral("Автодополнение:")));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("Сохраняй язык")));
    QVERIFY(!prompt.systemPrompt.contains(QStringLiteral("Autocomplete:")));
    // Контекст ровно один раз и без изменений — то, что просил ТЗ.
    QCOMPARE(prompt.userPrompt, input.context);
    QVERIFY(!prompt.userPrompt.startsWith(QLatin1Char('"')));
    QVERIFY(!prompt.systemPrompt.contains(input.context));
}

// Английский документ: английский шаблон правил (продолжение на языке
// контекста) и дословный контекст в user.
void PromptBuilderTest::englishContextKeepsLanguage()
{
    const PromptInput input = makeEnglishInput();

    const AutocompletePrompt prompt = PromptBuilder::build(input);

    QVERIFY(prompt.systemPrompt.startsWith(QStringLiteral("Autocomplete:")));
    QVERIFY(prompt.systemPrompt.contains(QStringLiteral("Keep the language")));
    QVERIFY(prompt.systemPrompt.contains(
        QStringLiteral("continuation only")));
    QVERIFY(prompt.systemPrompt.contains(
        QStringLiteral("no explanations")));
    QVERIFY(!prompt.systemPrompt.contains(QStringLiteral("Автодополнение")));
    QCOMPARE(prompt.userPrompt, input.context);
}

// Автоопределение языка: пустое поле language -> кириллица в контексте
// даёт русский шаблон, латиница/пустота — английский.
void PromptBuilderTest::languageAutoDetectsFromContext()
{
    PromptInput russian;
    russian.context = QStringLiteral("Ничего особенного, просто текст");
    QVERIFY(PromptBuilder::build(russian).systemPrompt.startsWith(
        QStringLiteral("Автодополнение:")));

    PromptInput english;
    english.context = QStringLiteral("Nothing special, just text");
    QVERIFY(PromptBuilder::build(english).systemPrompt.startsWith(
        QStringLiteral("Autocomplete:")));

    // Явный язык имеет приоритет над автоопределением.
    PromptInput forced = makeRussianInput();
    forced.language = QStringLiteral("en");
    QVERIFY(PromptBuilder::build(forced).systemPrompt.startsWith(
        QStringLiteral("Autocomplete:")));

    // Локаль вида «ru-RU» распознаётся как русский.
    PromptInput locale = makeRussianInput();
    locale.language = QStringLiteral("ru-RU");
    QVERIFY(PromptBuilder::build(locale).systemPrompt.startsWith(
        QStringLiteral("Автодополнение:")));
}

// Пустой контекст: сборка не падает, правила остаются (язык берётся из
// поля), user пуст, параметры проходят насквозь; выход детерминирован.
void PromptBuilderTest::emptyContextStillBuildsPrompt()
{
    PromptInput input;
    input.language = QStringLiteral("ru");
    input.maxTokens = 32;
    input.temperature = 0.5;
    input.topP = 0.8;

    const AutocompletePrompt prompt = PromptBuilder::build(input);

    QVERIFY(prompt.userPrompt.isEmpty());
    QVERIFY(prompt.systemPrompt.startsWith(QStringLiteral("Автодополнение:")));
    QVERIFY(!prompt.systemPrompt.contains(QStringLiteral("Стили:")));
    QCOMPARE(prompt.maxTokens, 32);
    QVERIFY(qFuzzyCompare(prompt.temperature, 0.5));
    QVERIFY(qFuzzyCompare(prompt.topP, 0.8));

    // Детерминизм: тот же пустой вход -> идентичный выход.
    const AutocompletePrompt again = PromptBuilder::build(input);
    QCOMPARE(again.systemPrompt, prompt.systemPrompt);
    QCOMPARE(again.userPrompt, prompt.userPrompt);
}

// Контекст не повторяется и не декорируется: user == контекст байт в
// байт, в system его нет ни в каком виде (в т.ч. без кавычек/меток).
void PromptBuilderTest::contextIsNeverDuplicatedOrDecorated()
{
    PromptInput input;
    input.context = QStringLiteral(
        "Уважаемые коллеги, приглашаем вас на совещание в четверг");
    input.language = QStringLiteral("ru");
    input.styles = QVector<StyleProfile>{activeProfile(0, 1.0)};

    const AutocompletePrompt prompt = PromptBuilder::build(input);

    // Ровно одно вхождение контекста — в user prompt, дословно.
    QCOMPARE(prompt.userPrompt, input.context);
    QVERIFY(!prompt.systemPrompt.contains(input.context));
    QVERIFY(!prompt.systemPrompt.contains(
        QStringLiteral("Уважаемые коллеги")));
    QVERIFY(!prompt.userPrompt.startsWith(QLatin1Char('"')));
    QVERIFY(!prompt.userPrompt.startsWith(QStringLiteral("Текст:")));
}

// Детерминированная структура: 20 подряд сборок дают идентичные
// system/user промпты (никаких дат, случайностей и событий UI).
void PromptBuilderTest::structureIsDeterministic()
{
    PromptInput input;
    input.context = QStringLiteral("Один и тот же вход");
    input.language = QStringLiteral("ru");
    input.styles = QVector<StyleProfile>{
        activeProfile(0, 0.6),
        activeProfile(1, 0.3),
        activeProfile(3, 0.1),
    };

    const AutocompletePrompt first = PromptBuilder::build(input);
    for (int i = 0; i < 20; ++i) {
        const AutocompletePrompt again = PromptBuilder::build(input);
        QCOMPARE(again.systemPrompt, first.systemPrompt);
        QCOMPARE(again.userPrompt, first.userPrompt);
        QCOMPARE(again.maxTokens, first.maxTokens);
    }
}

// Параметры генерации санируются диапазонами AppSettings: мусор из
// пользовательских настроек не доходит до backend'а (защита в глубину).
void PromptBuilderTest::generationParamsAreSanitized()
{
    PromptInput input;
    input.language = QStringLiteral("en");
    input.maxTokens = 99999;
    input.temperature = 100.0;
    input.topP = -3.0;

    AutocompletePrompt prompt = PromptBuilder::build(input);
    QCOMPARE(prompt.maxTokens, AppSettings::maxTokensMax);
    QVERIFY(qFuzzyCompare(prompt.temperature, AppSettings::temperatureMax));
    QVERIFY(qFuzzyCompare(prompt.topP, AppSettings::topPMin));

    input.maxTokens = 0;
    input.temperature = -1.0;
    input.topP = 5.0;
    prompt = PromptBuilder::build(input);
    QCOMPARE(prompt.maxTokens, AppSettings::maxTokensMin);
    QVERIFY(qFuzzyCompare(prompt.temperature, AppSettings::temperatureMin));
    QVERIFY(qFuzzyCompare(prompt.topP, AppSettings::topPMax));

    // Валидные значения проходят без изменений.
    input.maxTokens = 48;
    input.temperature = 0.3;
    input.topP = 0.7;
    prompt = PromptBuilder::build(input);
    QCOMPARE(prompt.maxTokens, 48);
    QVERIFY(qFuzzyCompare(prompt.temperature, 0.3));
    QVERIFY(qFuzzyCompare(prompt.topP, 0.7));
}

QTEST_GUILESS_MAIN(PromptBuilderTest)

#include "prompt_builder_test.moc"
