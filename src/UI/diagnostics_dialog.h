// diagnostics_dialog.h
#pragma once

#include "backend/text_generation_backend.h"

#include <QDialog>

class QPushButton;

// Режим диагностики backend'а: снимок BackendDiagnostics в виде списка
// строк + кнопка копирования текстового отчёта в буфер обмена.
//
// Отображаются ТОЛЬКО поля снимка (имя backend'а, имя модели, n_ctx,
// GPU-слои, токены, времена, токенов/с, последняя ошибка): сырые
// указатели, адреса и внутренние структуры backend'а в текст не
// попадают — форматирование идёт через formatReport() (контролируется
// тестом).
//
// Снимок фиксируется при открытии (подпись «Снимок на момент
// открытия»): показать новые цифры — закрыть и открыть диалог снова
// после новых запросов.
//
// Сборка: файл вырезается из сборки опцией PTUCH_DIAGNOSTICS=OFF
// (по умолчанию выключен в Release; кнопка Diagnostics в MainWindow
// и тесты диалога — тоже под этим макросом).
class DiagnosticsDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit DiagnosticsDialog(const BackendDiagnostics& data,
                               QWidget* parent = nullptr);

    // Текст для кнопки «Копировать»: строго из полей data — без
    // указателей, адресов и прочих внутренних значений.
    static QString formatReport(const BackendDiagnostics& data);

private slots:
    void copyReport();

private:
    // Отчёт копируется целиком (собирается один раз в конструкторе).
    QString m_report;
    QPushButton* m_copyButton = nullptr;
};
