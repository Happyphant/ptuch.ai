// style_profile.cpp
#include "style_profile.h"

QVector<StyleProfile> builtinStyleProfiles()
{
    QVector<StyleProfile> profiles;
    profiles.reserve(4);

    // Исторический автор (1799–1837): имя — описательное название,
    // см. юридическую оговорку в style_profile.h.
    StyleProfile pushkin;
    pushkin.id = QStringLiteral("pushkin");
    pushkin.displayName = QStringLiteral("Пушкин");
    pushkin.description = QStringLiteral(
        "Золотой век русской поэзии: ясность, гармония, светская "
        "интонация и лёгкая ирония.");
    pushkin.color = QColor(0x8e, 0x2d, 0x3f); // бордо
    pushkin.promptInstruction = QStringLiteral(
        "Пиши ясно и мелодично, с точной образностью, лёгкой иронией "
        "и завершённой фразой.");
    profiles.append(pushkin);

    // Исторический автор (1803–1873) — см. style_profile.h.
    StyleProfile tyutchev;
    tyutchev.id = QStringLiteral("tyutchev");
    tyutchev.displayName = QStringLiteral("Тютчев");
    tyutchev.description = QStringLiteral(
        "Философская лирика: природа, контрасты света и тьмы, "
        "возвышенная интонация.");
    tyutchev.color = QColor(0x2e, 0x4a, 0x7a); // тёмно-синий
    tyutchev.promptInstruction = QStringLiteral(
        "Используй возвышенную, философскую лексику и контрастные "
        "образы природы.");
    profiles.append(tyutchev);

    // Нейтральное описательное имя (жанр 80-х) — не бренд, см.
    // юридическую оговорку в style_profile.h.
    StyleProfile cyberpunk;
    cyberpunk.id = QStringLiteral("cyberpunk80");
    cyberpunk.displayName = QStringLiteral("Киберпанк 80-х");
    cyberpunk.description = QStringLiteral(
        "Неоновая антиутопия восьмидесятых: корпорации, дождь, "
        "уличный жаргон.");
    cyberpunk.color = QColor(0xc7, 0x29, 0xb4); // неоновая маджента
    cyberpunk.promptInstruction = QStringLiteral(
        "Пиши с неоновой антиутопической интонацией: техно-жаргон, "
        "корпоративный канцелярит, мрачный юмор.");
    profiles.append(cyberpunk);

    // Уже нейтральное описательное имя — как в примере ТЗ.
    StyleProfile official;
    official.id = QStringLiteral("official");
    official.displayName = QStringLiteral("Официальный стиль");
    official.description = QStringLiteral(
        "Деловой нейтральный тон: точность, вежливость, ясность.");
    official.color = QColor(0x4a, 0x67, 0x85); // стально-синий
    official.promptInstruction = QStringLiteral(
        "Пиши сухо, вежливо и однозначно: деловой тон, короткие "
        "предложения, без метафор.");
    profiles.append(official);

    return profiles;
}
