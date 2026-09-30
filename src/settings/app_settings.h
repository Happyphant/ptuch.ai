// app_settings.h
#pragma once

#include <QString>

// Настройки приложения — единый источник ключей QSettings, значений по
// умолчанию и допустимых диапазонов.
//
// Хранение: QSettings(PtuchAI/PtuchEditor) — тот же файл и тот же ключ
// llama/modelPath, что читает resolveModelPath() (см. llama_backend.cpp).
//
// Защита от некорректных значений (три уровня, защита в глубину):
//  1) SettingsDialog: QSpinBox/QDoubleSpinBox ограничены диапазонами
//     из этой структуры — полем выйти за границы нельзя;
//  2) load()/save() санируют через sanitize() — правленый руками файл
//     настроек не доставит мусор в контроллер/LlamaBackend;
//  3) LlamaBackend дополнительно клампит n_ctx/maxTokens/temperature.
struct AppSettings
{
    // llama.cpp
    QString modelPath;        // пусто = авто-поиск (env -> models/)
    int contextSize = 4096;
    int maxTokens = 64;
    double temperature = 0.7;
    double topP = 0.9;
    int gpuLayers = -1;       // -1 = все слои на GPU

    // Подсказки
    int debounceMs = 500;
    bool autoSuggestions = true;

    // Тема: схема оформления (см. src/UI/theme.h). "default" —
    // системные цвета шрифтов и контролов, "ptuch" — тёмная тема
    // ПТЮЧ.AI (дефолт), "light" — светлая. Допустимые id — константы
    // style* ниже; sanitize() отбрасывает прочие значения.
    QString style = QLatin1String(stylePtuch);

    // Диапазоны — единый источник истины для виджетов и sanitize().
    static constexpr int contextSizeMin = 256;
    static constexpr int contextSizeMax = 8192;
    static constexpr int maxTokensMin = 1;
    static constexpr int maxTokensMax = 512;
    static constexpr double temperatureMin = 0.1;
    static constexpr double temperatureMax = 2.0;
    static constexpr double topPMin = 0.1;
    static constexpr double topPMax = 1.0;
    static constexpr int gpuLayersMin = -1;
    static constexpr int gpuLayersMax = 128;
    static constexpr int debounceMin = 50;
    static constexpr int debounceMax = 10000;

    // Ключи QSettings (организация/приложение задаются в main.cpp и
    // продублированы здесь — resolveModelPath читает их напрямую).
    static constexpr const char* keyModelPath = "llama/modelPath";
    static constexpr const char* keyContextSize = "llama/contextSize";
    static constexpr const char* keyMaxTokens = "llama/maxTokens";
    static constexpr const char* keyTemperature = "llama/temperature";
    static constexpr const char* keyTopP = "llama/topP";
    static constexpr const char* keyGpuLayers = "llama/gpuLayers";
    static constexpr const char* keyDebounceMs = "suggestion/debounceMs";
    static constexpr const char* keyAutoSuggestions =
        "suggestion/autoSuggestions";
    // Схема темы: id — styleSystem/stylePtuch/styleLight.
    static constexpr const char* keyStyle = "ui/style";
    static constexpr const char* styleSystem = "default";
    static constexpr const char* stylePtuch = "ptuch";
    static constexpr const char* styleLight = "light";

    // Чтение из QSettings + санитизация диапазонами: возвращаемое
    // значение ВСЕГДА валидно для контроллера и LlamaBackend.
    static AppSettings load();
    // Запись всех ключей (значения санируются перед записью).
    void save() const;
    // Обрезка всех числовых полей до допустимых диапазонов.
    void sanitize();
};
