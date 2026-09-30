// prompt_builder.cpp
#include "prompt_builder.h"

#include "settings/app_settings.h" // диапазоны параметров генерации
#include "suggestion/style_mixer.h"

namespace {

// Фиксированные шаблоны правил — без подстановок и без контекста
// документа: короткие, одинаковые для одинакового языка (п.1, п.5
// контракта в prompt_builder.h).
QString rulesTemplate(const QString& language)
{
    if (language == QLatin1String("ru")) {
        return QStringLiteral(
            "Автодополнение: продолжи текст строго после последнего "
            "символа, не повторяя уже введённый контекст. Ответ — "
            "только продолжение: без пояснений, кавычек, вопросов и "
            "метакомментариев. Сохраняй язык, стиль и форматирование "
            "документа.");
    }
    return QStringLiteral(
        "Autocomplete: continue the text strictly after the last "
        "character, without repeating the already typed context. "
        "Reply with the continuation only — no explanations, quotes, "
        "questions, or meta-commentary. Keep the language, style, "
        "and formatting of the document.");
}

// Заголовок блока стилей — на языке шаблона правил.
QString stylesHeader(const QString& language)
{
    return language == QLatin1String("ru") ? QStringLiteral("Стили:")
                                           : QStringLiteral("Styles:");
}

} // namespace

QString PromptBuilder::resolveLanguage(const QString& language,
                                       const QString& context)
{
    const QString normalized = language.trimmed().toLower();
    if (normalized == QLatin1String("ru")
        || normalized.startsWith(QLatin1String("ru-"))) {
        return QStringLiteral("ru");
    }
    if (normalized == QLatin1String("en")
        || normalized.startsWith(QLatin1String("en-"))) {
        return QStringLiteral("en");
    }

    // Автоопределение по символам контекста: кириллица -> русский;
    // пустой или латинский контекст -> английский (нейтральный
    // дефолт для модели).
    for (const QChar ch : context) {
        if (ch.script() == QChar::Script_Cyrillic) {
            return QStringLiteral("ru");
        }
    }
    return QStringLiteral("en");
}

AutocompletePrompt PromptBuilder::build(const PromptInput& input)
{
    AutocompletePrompt prompt;

    const QString language = resolveLanguage(input.language, input.context);

    // 1) system: фиксированные правила [+ блок стилей]. Контекст
    //    документа сюда НЕ попадает — он не должен повторяться.
    QString system = rulesTemplate(language);
    const QString styleInstructions =
        StyleMixer(input.styles).buildInstruction();
    if (!styleInstructions.isEmpty()) {
        system += QLatin1Char('\n');
        system += stylesHeader(language);
        system += QLatin1Char('\n');
        system += styleInstructions;
    }
    prompt.systemPrompt = system;

    // 2) user: контекст дословно — один раз, без кавычек, меток и
    //    пояснений (их запрет живёт в правилах system'а).
    prompt.userPrompt = input.context;

    // 3) Параметры: санируем границами AppSettings — мусор из
    //    пользовательских настроек не доедет до backend'а (защита в
    //    глубину наряду с SettingsDialog/LlamaBackend).
    prompt.maxTokens = qBound(AppSettings::maxTokensMin, input.maxTokens,
                              AppSettings::maxTokensMax);
    prompt.temperature = qBound(AppSettings::temperatureMin,
                                input.temperature,
                                AppSettings::temperatureMax);
    prompt.topP = qBound(AppSettings::topPMin, input.topP,
                         AppSettings::topPMax);

    return prompt;
}
