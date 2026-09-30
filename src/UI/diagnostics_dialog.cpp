// diagnostics_dialog.cpp
#include "diagnostics_dialog.h"

#include "settings/app_settings.h"
#include "theme.h"

#include <QClipboard>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
// Значение отсутствует: модель не настроена/не загружена, запросов не
// было. Отдельный символ — чтобы не путать с настоящим нулём.
QString noValue()
{
    return QStringLiteral("—");
}

// Поле снимка -> строка отображения. Одни и те же функции используются
// и метками диалога, и текстом отчёта — значения не расходятся.
QString formatBackendName(const BackendDiagnostics& data)
{
    return data.backendName.isEmpty() ? noValue() : data.backendName;
}

QString formatModel(const BackendDiagnostics& data)
{
    return data.modelName.isEmpty() ? noValue() : data.modelName;
}

QString formatContext(const BackendDiagnostics& data)
{
    return data.contextSize > 0 ? QString::number(data.contextSize)
                                : noValue();
}

// GPU-слои имеют смысл только когда модель известна (иначе «—»);
// -1 = все слои на GPU.
QString formatGpuLayers(const BackendDiagnostics& data)
{
    if (data.modelName.isEmpty())
        return noValue();
    return data.gpuLayers < 0 ? QStringLiteral("-1 (все слои)")
                              : QString::number(data.gpuLayers);
}

QString formatCount(qint64 value)
{
    return value > 0 ? QString::number(value) : noValue();
}

QString formatMs(qint64 value)
{
    return value > 0 ? QString::number(value) + QStringLiteral(" мс")
                     : noValue();
}

// Токенов в секунду: generated / generationTime (считается при
// отображении — источник истин только поля снимка).
QString formatTps(const BackendDiagnostics& data)
{
    if (data.generationMs <= 0 || data.generatedTokens <= 0)
        return noValue();
    const double tps = static_cast<double>(data.generatedTokens) * 1000.0
        / static_cast<double>(data.generationMs);
    return QString::number(tps, 'f', 1);
}

QString formatError(const BackendDiagnostics& data)
{
    return data.lastError.isEmpty() ? QStringLiteral("нет ошибок")
                                    : data.lastError;
}
} // namespace

DiagnosticsDialog::DiagnosticsDialog(const BackendDiagnostics& data,
                                     QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("diagnosticsDialog"));
    setWindowTitle(tr("Диагностика backend"));
    setModal(true);

    // Тема распространяется на диалог (наследование от MainWindow);
    // собственный stylesheet задаёт фон и состояния контролов — по
    // схеме из настроек (default — пустой, системный вид).
    const Theme::Scheme scheme =
        Theme::schemeFromId(AppSettings::load().style);
    setStyleSheet(Theme::dialogStyleSheet(scheme));
    const Theme::Colors colors = Theme::colors(scheme);

    m_report = formatReport(data);

    auto* mainLayout = new QVBoxLayout(this);

    // Снимок фиксируется при открытии: новые цифры — после новых
    // запросов (закрыть и открыть снова).
    auto* note = new QLabel(tr("Снимок на момент открытия"), this);
    note->setObjectName(QStringLiteral("snapshotNote"));
    // Вторичный цвет схемы; системная схема не даёт цвета (invalid) —
    // метка остаётся со системным шрифтом.
    if (colors.dimTextColor.isValid()) {
        note->setStyleSheet(
            QStringLiteral("color: %1;")
                .arg(colors.dimTextColor.name(QColor::HexRgb)));
    }
    mainLayout->addWidget(note);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    form->setContentsMargins(0, 0, 0, 0);

    const auto addRow = [this, form](const QString& labelText,
                                     const QString& objectName,
                                     const QString& value) {
        auto* label = new QLabel(value, this);
        label->setObjectName(objectName);
        // Значения можно выделять мышью (удобно для длинных имён).
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(labelText, label);
    };

    addRow(tr("Бэкенд:"), QStringLiteral("backendValue"),
           formatBackendName(data));
    addRow(tr("Имя модели:"), QStringLiteral("modelValue"),
           formatModel(data));
    addRow(tr("Размер контекста:"), QStringLiteral("contextValue"),
           formatContext(data));
    addRow(tr("GPU-слои:"), QStringLiteral("gpuLayersValue"),
           formatGpuLayers(data));
    addRow(tr("Prompt tokens:"), QStringLiteral("promptTokensValue"),
           formatCount(data.promptTokens));
    addRow(tr("Generated tokens:"),
           QStringLiteral("generatedTokensValue"),
           formatCount(data.generatedTokens));
    addRow(tr("Обработка prompt:"), QStringLiteral("promptTimeValue"),
           formatMs(data.promptProcessingMs));
    addRow(tr("Генерация:"), QStringLiteral("generationTimeValue"),
           formatMs(data.generationMs));
    addRow(tr("Токенов в секунду:"), QStringLiteral("tpsValue"),
           formatTps(data));

    // Последняя ошибка: возможен длинный текст — переносим; красный
    // цвет схемы — только при реальной ошибке (системная схема цвет
    // не задаёт).
    auto* errorLabel = new QLabel(formatError(data), this);
    errorLabel->setObjectName(QStringLiteral("errorValue"));
    errorLabel->setWordWrap(true);
    errorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (!data.lastError.isEmpty() && colors.error.isValid()) {
        errorLabel->setStyleSheet(
            QStringLiteral("color: %1;")
                .arg(colors.error.name(QColor::HexRgb)));
    }
    form->addRow(tr("Последняя ошибка:"), errorLabel);

    mainLayout->addLayout(form);
    mainLayout->addStretch(1);

    auto* buttons = new QHBoxLayout;
    m_copyButton = new QPushButton(tr("Копировать"), this);
    m_copyButton->setObjectName(QStringLiteral("copyButton"));
    m_copyButton->setToolTip(
        tr("Скопировать отчёт диагностики в буфер обмена"));
    connect(m_copyButton, &QPushButton::clicked,
            this, &DiagnosticsDialog::copyReport);

    auto* closeButton = new QPushButton(tr("Закрыть"), this);
    closeButton->setObjectName(QStringLiteral("closeButton"));
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    buttons->addStretch(1);
    buttons->addWidget(m_copyButton);
    buttons->addWidget(closeButton);
    mainLayout->addLayout(buttons);
}

QString DiagnosticsDialog::formatReport(const BackendDiagnostics& data)
{
    // Строго из полей снимка (whitelist): никаких указателей, адресов
    // и внутренних структур backend'а — это проверяется тестом.
    const QStringList lines = {
        QStringLiteral("Ptuch Editor — диагностика backend"),
        QStringLiteral("Бэкенд: %1").arg(formatBackendName(data)),
        QStringLiteral("Имя модели: %1").arg(formatModel(data)),
        QStringLiteral("Размер контекста: %1").arg(formatContext(data)),
        QStringLiteral("GPU-слои: %1").arg(formatGpuLayers(data)),
        QStringLiteral("Prompt tokens: %1")
            .arg(formatCount(data.promptTokens)),
        QStringLiteral("Generated tokens: %1")
            .arg(formatCount(data.generatedTokens)),
        QStringLiteral("Обработка prompt: %1")
            .arg(formatMs(data.promptProcessingMs)),
        QStringLiteral("Генерация: %1").arg(formatMs(data.generationMs)),
        QStringLiteral("Токенов в секунду: %1").arg(formatTps(data)),
        QStringLiteral("Последняя ошибка: %1").arg(formatError(data)),
    };
    return lines.join(QLatin1Char('\n'));
}

void DiagnosticsDialog::copyReport()
{
    QGuiApplication::clipboard()->setText(m_report);
    // Краткая обратная связь: кнопка подтверждает копирование.
    m_copyButton->setText(tr("Скопировано"));
}
