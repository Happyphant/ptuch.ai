// app_settings.cpp
#include "app_settings.h"

#include <QSettings>

// Организация/приложение продублированы (как в resolveModelPath()):
// файл настроек один на всех, независимо от имён в main.cpp.
AppSettings AppSettings::load()
{
    AppSettings value;
    const QSettings settings(QStringLiteral("PtuchAI"),
                             QStringLiteral("PtuchEditor"));

    // Отсутствующий ключ -> дефолт структуры; мусор (например, "abc"
    // вместо числа) превратится в 0/0.0 и будет обрезан sanitize().
    value.modelPath = settings.value(keyModelPath).toString();
    value.contextSize =
        settings.value(keyContextSize, value.contextSize).toInt();
    value.maxTokens =
        settings.value(keyMaxTokens, value.maxTokens).toInt();
    value.temperature =
        settings.value(keyTemperature, value.temperature).toDouble();
    value.topP = settings.value(keyTopP, value.topP).toDouble();
    value.gpuLayers =
        settings.value(keyGpuLayers, value.gpuLayers).toInt();
    value.debounceMs =
        settings.value(keyDebounceMs, value.debounceMs).toInt();
    value.autoSuggestions =
        settings.value(keyAutoSuggestions, value.autoSuggestions)
            .toBool();
    value.style = settings.value(keyStyle, value.style).toString();

    value.sanitize();
    return value;
}

void AppSettings::save() const
{
    // Санируем копь: в файл не попадает ничего за границами диапазонов
    // (load() санирует повторно — правленый руками файл тоже безопасен).
    AppSettings value = *this;
    value.sanitize();

    QSettings settings(QStringLiteral("PtuchAI"),
                       QStringLiteral("PtuchEditor"));
    settings.setValue(keyModelPath, value.modelPath);
    settings.setValue(keyContextSize, value.contextSize);
    settings.setValue(keyMaxTokens, value.maxTokens);
    settings.setValue(keyTemperature, value.temperature);
    settings.setValue(keyTopP, value.topP);
    settings.setValue(keyGpuLayers, value.gpuLayers);
    settings.setValue(keyDebounceMs, value.debounceMs);
    settings.setValue(keyAutoSuggestions, value.autoSuggestions);
    settings.setValue(keyStyle, value.style);
    // Гарантия записи до возврата — настройки переживут перезапуск
    // даже при аварийном завершении процесса.
    settings.sync();
}

void AppSettings::sanitize()
{
    contextSize = qBound(contextSizeMin, contextSize, contextSizeMax);
    maxTokens = qBound(maxTokensMin, maxTokens, maxTokensMax);
    temperature = qBound(temperatureMin, temperature, temperatureMax);
    topP = qBound(topPMin, topP, topPMax);
    gpuLayers = qBound(gpuLayersMin, gpuLayers, gpuLayersMax);
    debounceMs = qBound(debounceMin, debounceMs, debounceMax);

    // Схема темы: правленый/мусорный id -> дефолт ПТЮЧ
    // (те же id, что Theme::schemeFromId).
    if (style != QLatin1String(styleSystem) &&
        style != QLatin1String(stylePtuch) &&
        style != QLatin1String(styleLight))
        style = QLatin1String(stylePtuch);
}
