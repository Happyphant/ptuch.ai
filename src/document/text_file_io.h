// text_file_io.h
#pragma once

#include <QString>

// Результат чтения текстового файла.
struct TextLoadResult
{
    bool ok = false;
    QString text;  // содержимое (только при ok); ведущий UTF-8 BOM отброшен
    QString error; // описание ошибки (только при !ok) — для диалога
};

// Результат атомарной записи текстового файла.
struct TextSaveResult
{
    bool ok = false;
    QString error; // описание ошибки (только при !ok) — для диалога
};

// Файловый слой документа v1: plain text в кодировке UTF-8 (без BOM).
// Только Qt Core — без виджетов: диалоги ошибок рисует вызывающий
// (MainWindow), сюда приходят только пути и текст.
//
// Запись — через QSaveFile: данные сначала уходят во временный файл в
// той же папке и переименовываются ТОЛЬКО при успешном commit() —
// ошибка записи/обрыв не портят уже существующий файл.
class TextFileIO
{
public:
    // Чтение целиком. Ошибки: файл не существует/не читаем (нет прав,
    // это каталог), сбой чтения — всё в TextLoadResult::error.
    // Невалидные байты не роняют чтение (замена на U+FFFD — правила
    // QString::fromUtf8); русский текст проходит без потерь.
    static TextLoadResult loadUtf8(const QString& path);

    // Атомарная запись UTF-8 (без BOM): text.toUtf8() -> QSaveFile.
    // Ошибки: каталог не существует/не записываем, сбой записи,
    // неудачный commit (переименование).
    static TextSaveResult saveUtf8(const QString& path,
                                   const QString& text);
};
