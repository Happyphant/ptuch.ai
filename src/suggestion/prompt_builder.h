// prompt_builder.h
#pragma once

#include "suggestion/style_profile.h"

#include <QString>
#include <QVector>

// Вход PromptBuilder для режима автодополнения — всё, что нужно, чтобы
// собрать промпт; значения, а не виджеты/QSettings (вызывающая сторона
// читает настройки сама).
struct PromptInput
{
    // Обрезанный контекст документа до курсора — тот же смысл, что в
    // GenerationRequest::context. Единственный текст, уходящий в
    // user prompt (и ровно один раз).
    QString context;
    // Активные стилевые профили (с весами). Неактивные/нулевые
    // отфильтрует StyleMixer — «сырой» список тоже безопасен.
    QVector<StyleProfile> styles;
    // Язык документа: «ru»/«en» (и локали вида «ru-RU»). Пусто или
    // неизвестно -> автоопределение по символам контекста.
    QString language;
    // Параметры генерации (пользовательские настройки) — санируются
    // границами AppSettings прямо в сборке.
    int maxTokens = 64;
    double temperature = 0.7;
    double topP = 0.9;
};

// Результат сборки: system prompt + user prompt + параметры генерации.
// Поля 1:1 ложатся в GenerationRequest: systemPrompt -> systemPrompt,
// userPrompt -> context, параметры -> maxTokens/temperature/topP.
struct AutocompletePrompt
{
    QString systemPrompt;
    QString userPrompt;
    int maxTokens = 64;
    double temperature = 0.7;
    double topP = 0.9;
};

// PromptBuilder — детерминированный сборщик промпта автодополнения.
// Чистая функция: без состояния, виджетов и QSettings — тот же вход
// даёт байт в байт тот же выход.
//
// Контракт промпта (закреплён unit-тестами):
//  1. system просит ТОЛЬКО продолжение текста и прямо запрещает
//     пояснения, кавычки, вопросы и метакомментарии — фиксированный
//     короткий шаблон без подстановок;
//  2. введённый контекст НЕ повторяется: он входит ровно один раз —
//     в user prompt, дословно, без кавычек, меток и пояснений;
//     systemPrompt контекста не содержит вовсе;
//  3. язык документа сохраняется: шаблон правил выбирается по полю
//     language; при пустом/неизвестном языке — автоопределение по
//     символам контекста (кириллица -> ru, иначе en);
//  4. смешанные стили учитываются блоком «Стили:/Styles:» — все
//     активные профили с нормализованными весами (формат
//     StyleMixer::buildInstruction: «Имя (0.67): инструкция»);
//  5. структура детерминирована и короткая: правила [затем блок
//     стилей], никаких дат, случайных элементов и зависимостей от
//     событий UI.
//
// Отношение к адаптерам (LoRA): смешивание стилей через prompt — это
// MVP-механизм; настоящее наложение LoRA-адаптеров требует отдельного
// inference backend'а (см. README «Стили и LoRA-адаптеры»). Поэтому
// поля StyleProfile adapterPath/adapterType/baseModelId/promptTag/
// adapterScale в сборку промпта НЕ входят (promptTag — часть
// adapter-пути, а не prompt-текста), а сам PromptBuilder зависит
// только от нейтральных Qt-полей профиля — никаких типов и терминов
// библиотек обучения (см. style_profile.h).
class PromptBuilder final
{
public:
    static AutocompletePrompt build(const PromptInput& input);

private:
    // «ru»/«en»: явный язык (в т.ч. «ru-RU») -> он; иначе
    // автоопределение по контексту (см. п.3 контракта).
    static QString resolveLanguage(const QString& language,
                                   const QString& context);
};
