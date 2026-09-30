// text_file_io.cpp
#include "text_file_io.h"

#include <QFile>
#include <QSaveFile>

TextLoadResult TextFileIO::loadUtf8(const QString& path)
{
    TextLoadResult result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }

    const QByteArray raw = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        // Сбой чтения посреди файла (например, это каталог) —
        // результат не выдаём, чтобы не «сохранить» обрывок.
        result.error = file.errorString();
        return result;
    }

    QString text = QString::fromUtf8(raw);
    // Ведущий UTF-8 BOM не должен стать символом в первом абзаце.
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);

    result.ok = true;
    result.text = text;
    return result;
}

TextSaveResult TextFileIO::saveUtf8(const QString& path,
                                    const QString& text)
{
    TextSaveResult result;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = file.errorString();
        return result;
    }

    // UTF-8 без BOM: русский текст сохраняется байтами UTF-8.
    const QByteArray data = text.toUtf8();
    if (file.write(data) != data.size()) {
        const QString writeError = file.errorString();
        file.cancelWriting(); // temp-файл откатывается, оригинал цел
        result.error = writeError;
        return result;
    }

    if (!file.commit()) {
        // Переименование не удалось: temp-файл отброшен, оригинал цел.
        result.error = file.errorString();
        return result;
    }

    result.ok = true;
    return result;
}
