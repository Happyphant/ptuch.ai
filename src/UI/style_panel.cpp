// style_panel.cpp
#include "style_panel.h"

#include "theme.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace {

// Числа панели — 2 знака (точно как доли в prompt-инструкции
// StyleMixer::buildInstruction).
QString formatWeight(double weight)
{
    return QString::number(weight, 'f', 2);
}

} // namespace

StylePanel::StylePanel(QWidget* parent)
    : StylePanel(StyleMixer(), parent)
{
}

StylePanel::StylePanel(StyleMixer mixer, QWidget* parent)
    : QWidget(parent)
    , m_mixer(std::move(mixer))
{
    setObjectName(QStringLiteral("stylePanel"));
    // Условие отрисовки фона из stylesheet для собственного класса.
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(340);

    buildUi();
    refreshLabels(); // стартовое состояние — без сигналов (до connect)
    applyPanelTheme(); // stylesheet панели + маркеры профилей
}

void StylePanel::setScheme(Theme::Scheme scheme)
{
    if (m_scheme == scheme)
        return;
    m_scheme = scheme;
    applyPanelTheme();
}

void StylePanel::applyPanelTheme()
{
    setStyleSheet(Theme::panelStyleSheet(m_scheme));

    // Цветовой маркер: цвет профиля + рамка/радиус схемы (у
    // системной схемы — нейтральная рамка без радиуса).
    for (const StyleProfile& profile : m_mixer.profiles()) {
        auto* marker = findChild<QLabel*>(
            QStringLiteral("colorMarker_%1").arg(profile.id));
        if (marker != nullptr) {
            marker->setStyleSheet(Theme::colorMarkerStyle(
                profile.color, m_scheme));
        }
    }
}

void StylePanel::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(2, 1); // слайдер забирает свободную ширину
    root->addLayout(grid);
    // Свободная высота уходит сюда: строки — сверху, футер — снизу.
    root->addStretch(1);

    int rowIndex = 0;
    for (const StyleProfile& profile : m_mixer.profiles()) {
        // Цветовой маркер стиля (описание — в подсказке).
        auto* marker = new QLabel(this);
        marker->setObjectName(
            QStringLiteral("colorMarker_%1").arg(profile.id));
        marker->setFixedSize(14, 14);
        marker->setToolTip(profile.description);
        marker->setStyleSheet(
            Theme::colorMarkerStyle(profile.color, m_scheme));
        grid->addWidget(marker, rowIndex, 0, Qt::AlignVCenter);

        auto* nameLabel = new QLabel(profile.displayName, this);
        nameLabel->setObjectName(
            QStringLiteral("nameLabel_%1").arg(profile.id));
        nameLabel->setToolTip(profile.description);
        grid->addWidget(nameLabel, rowIndex, 1);

        Row row;
        row.slider = new QSlider(Qt::Horizontal, this);
        row.slider->setObjectName(
            QStringLiteral("slider_%1").arg(profile.id));
        row.slider->setRange(0, 100);
        row.slider->setMinimumWidth(110);
        row.slider->setToolTip(tr("Вес стиля: 0..100"));
        row.slider->setValue(
            qRound(profile.weight * 100.0)); // стартовое — ДО connect
        row.slider->setEnabled(profile.enabled);
        grid->addWidget(row.slider, rowIndex, 2);

        row.valueLabel = new QLabel(this);
        row.valueLabel->setObjectName(
            QStringLiteral("valueLabel_%1").arg(profile.id));
        row.valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row.valueLabel->setFixedWidth(30);
        row.valueLabel->setToolTip(tr("Значение слайдера"));
        grid->addWidget(row.valueLabel, rowIndex, 3);

        row.normalizedLabel = new QLabel(this);
        row.normalizedLabel->setObjectName(
            QStringLiteral("normalizedLabel_%1").arg(profile.id));
        row.normalizedLabel->setAlignment(
            Qt::AlignRight | Qt::AlignVCenter);
        row.normalizedLabel->setFixedWidth(40);
        row.normalizedLabel->setToolTip(
            tr("Нормализованный вес в миксе (сумма 1.0)"));
        grid->addWidget(row.normalizedLabel, rowIndex, 4);

        row.enabledCheck = new QCheckBox(this);
        row.enabledCheck->setObjectName(
            QStringLiteral("enabledCheck_%1").arg(profile.id));
        row.enabledCheck->setChecked(
            profile.enabled); // стартовое — ДО connect
        row.enabledCheck->setToolTip(tr("Включить стиль в микс"));
        grid->addWidget(row.enabledCheck, rowIndex, 5, Qt::AlignVCenter);

        auto* resetButton = new QPushButton(tr("Сброс"), this);
        resetButton->setObjectName(
            QStringLiteral("resetButton_%1").arg(profile.id));
        resetButton->setProperty("kind", "styleReset");
        resetButton->setToolTip(tr("Сбросить вес этого стиля"));
        grid->addWidget(resetButton, rowIndex, 6);

        m_rows.insert(profile.id, row);

        // --- Связи ПОСЛЕ инициализации виджетов: стартовые значения
        // сигналов не дают (построение панели — не «изменение микса»).
        const QString id = profile.id;

        connect(row.slider, &QSlider::valueChanged, this,
                [this, id](int value) {
                    m_mixer.setWeight(id, value / 100.0);
                    notifyMixChanged();
                });

        connect(row.enabledCheck, &QCheckBox::toggled, this,
                [this, id](bool checked) {
                    m_mixer.setEnabled(id, checked);
                    // Серый слайдер выключенного профиля: setEnabled
                    // сигналов не порождает — цикла нет.
                    m_rows.value(id).slider->setEnabled(checked);
                    notifyMixChanged();
                });

        connect(resetButton, &QPushButton::clicked, this, [this, id]() {
            // setValue -> valueChanged -> тот же единый путь: ровно
            // один сигнал. При уже нулевом весе изменения (и
            // сигнала) нет.
            m_rows.value(id).slider->setValue(0);
        });

        ++rowIndex;
    }

    // --- Нижняя строка: сумма активных весов + Reset Mix.
    auto* footer = new QHBoxLayout;

    m_sumLabel = new QLabel(this);
    m_sumLabel->setObjectName(QStringLiteral("sumLabel"));
    m_sumLabel->setToolTip(
        tr("Сумма весов активных профилей (слайдер 42 = вес 0.42)"));
    footer->addWidget(m_sumLabel);
    footer->addStretch(1);

    m_resetMixButton = new QPushButton(tr("Reset Mix"), this);
    m_resetMixButton->setObjectName(QStringLiteral("resetMixButton"));
    m_resetMixButton->setToolTip(tr("Обнулить веса всех стилей"));
    footer->addWidget(m_resetMixButton);
    root->addLayout(footer);

    connect(m_resetMixButton, &QPushButton::clicked, this, [this]() {
        // Один жест — один сигнал: программные setValue гасятся
        // QSignalBlocker'ом, подписи и эмиссия — общим notifyMixChanged.
        bool changed = false;
        for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
            if (it->slider->value() != 0) {
                const QSignalBlocker blocker(it->slider);
                it->slider->setValue(0);
                changed = true;
            }
            m_mixer.setWeight(it.key(), 0.0);
        }
        if (changed)
            notifyMixChanged();
        // Уже пустой микс — без изменений и без сигнала.
    });
}

void StylePanel::refreshLabels()
{
    // Нормализация — домен модели, панель только показывает результат.
    const QVector<StyleWeight> normalized = m_mixer.styleWeights();

    for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
        const Row& row = it.value();

        row.valueLabel->setText(QString::number(row.slider->value()));

        // Доля активного профиля в миксе; неактивный — «—».
        QString share = QStringLiteral("—");
        for (const StyleWeight& weight : normalized) {
            if (weight.styleId == it.key()) {
                share = formatWeight(double(weight.weight));
                break;
            }
        }
        row.normalizedLabel->setText(share);
    }

    m_sumLabel->setText(
        tr("Сумма: %1").arg(formatWeight(m_mixer.activeTotal())));
}

void StylePanel::notifyMixChanged()
{
    refreshLabels();
    emit styleMixChanged(m_mixer.styleWeights());
}
