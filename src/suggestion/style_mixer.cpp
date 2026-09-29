// style_mixer.cpp
#include "style_mixer.h"

#include <QStringList>

namespace {

// Сырые веса живут в диапазоне [0; 1] — как ползунок в UI. Отношения
// внутри диапазона задают пропорции, приведение к сумме 1.0 делает
// нормализация (см. header).
constexpr double kMinWeight = 0.0;
constexpr double kMaxWeight = 1.0;

bool isActive(const StyleProfile& profile)
{
    return profile.enabled && profile.weight > 0.0;
}

} // namespace

StyleMixer::StyleMixer()
    : StyleMixer(builtinStyleProfiles())
{
}

StyleMixer::StyleMixer(QVector<StyleProfile> profiles)
    : m_profiles(std::move(profiles))
{
    // Санитизация на входе: мусорные веса (NaN/>1/отрицательные) не
    // должны ломать нормализацию и prompt-инструкцию.
    for (StyleProfile& profile : m_profiles) {
        if (!(profile.weight >= kMinWeight))
            profile.weight = kMinWeight; // в т.ч. NaN
        else
            profile.weight = qBound(kMinWeight, profile.weight, kMaxWeight);
    }
}

const StyleProfile* StyleMixer::profile(const QString& id) const
{
    for (const StyleProfile& profile : m_profiles) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}

bool StyleMixer::setWeight(const QString& id, double weight)
{
    for (StyleProfile& profile : m_profiles) {
        if (profile.id == id) {
            profile.weight = qBound(kMinWeight, weight, kMaxWeight);
            return true;
        }
    }
    return false;
}

bool StyleMixer::setEnabled(const QString& id, bool enabled)
{
    for (StyleProfile& profile : m_profiles) {
        if (profile.id == id) {
            profile.enabled = enabled;
            return true;
        }
    }
    return false;
}

void StyleMixer::normalize()
{
    const double total = activeTotal();
    if (total <= 0.0)
        return; // активных нет — нечего нормализовать

    for (StyleProfile& profile : m_profiles) {
        if (isActive(profile))
            profile.weight /= total;
    }
}

QVector<StyleProfile> StyleMixer::activeProfiles() const
{
    QVector<StyleProfile> active;
    for (const StyleProfile& profile : m_profiles) {
        if (isActive(profile))
            active.append(profile);
    }
    return active;
}

QVector<StyleWeight> StyleMixer::styleWeights() const
{
    QVector<StyleWeight> weights;

    const double total = activeTotal();
    if (total <= 0.0)
        return weights; // активных нет — в запрос ничего не кладём

    for (const StyleProfile& profile : m_profiles) {
        if (!isActive(profile))
            continue;
        StyleWeight style;
        style.styleId = profile.id;
        style.weight = static_cast<float>(profile.weight / total);
        weights.append(style);
    }
    return weights;
}

QString StyleMixer::buildInstruction() const
{
    const double total = activeTotal();
    if (total <= 0.0)
        return QString(); // нулевой/пустой набор — пустой prompt-кусок

    QStringList lines;
    for (const StyleProfile& profile : m_profiles) {
        if (!isActive(profile))
            continue; // нулевой или выключенный — не попадает в prompt
        if (profile.promptInstruction.isEmpty())
            continue; // нечего добавлять — пустая строка не нужна

        const double normalized = profile.weight / total;
        lines << QStringLiteral("%1 (%2): %3")
                     .arg(profile.displayName,
                          QString::number(normalized, 'f', 2),
                          profile.promptInstruction);
    }
    return lines.join(QLatin1Char('\n'));
}

double StyleMixer::activeTotal() const
{
    double total = 0.0;
    for (const StyleProfile& profile : m_profiles) {
        if (isActive(profile))
            total += profile.weight;
    }
    return total;
}
