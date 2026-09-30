// text_file_io_test.cpp
#include <QtTest>

#include "document/text_file_io.h"

#include <QFile>
#include <QTemporaryDir>

// ---------------------------------------------------------------------------
// Файловый слой документа v1 (UTF-8 plain text, запись через QSaveFile):
//  - русский текст проходит round-trip БАЙТ В БАЙТ (UTF-8 без BOM);
//  - чтение несуществующего файла и каталога — ошибка, не «пустой успех»;
//  - запись в несуществующий каталог — ошибка, файл не создаётся;
//  - перезапись существующего файла атомарна (читается новое содержимое);
//  - ведущий UTF-8 BOM при чтении отбрасывается (не становится символом).
//
// QTEST_GUILESS_MAIN + только Qt6::Core: слой не знает про виджеты —
// диалоги ошибок рисует MainWindow.
// ---------------------------------------------------------------------------
class TextFileIOTest final : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsRussianTextAsUtf8();
    void overwritesExistingFileAtomically();
    void stripsLeadingBomOnLoad();
    void loadsMissingFileAsError();
    void loadsDirectoryAsError();
    void savesToMissingDirectoryAsError();
    void savesEmptyText();
};

namespace {

// Прямая запись сырых байтов (для подготовки условий/проверок).
bool writeRaw(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(data) == data.size();
}

QByteArray readRaw(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

// Русский текст: запись и чтение дают в точности исходную строку, а
// сырые байты файла — UTF-8 без BOM (в т.ч. кириллическое имя файла).
void TextFileIOTest::roundTripsRussianTextAsUtf8()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("заметки.txt"));
    const QString text = QStringLiteral(
        "Привет, мир! Русский текст: ёЁ№ «кавычки» — тире.\n"
        "Вторая строка с эмодзи ✅ и без него.");

    const TextSaveResult saved = TextFileIO::saveUtf8(path, text);
    QVERIFY2(saved.ok, qPrintable(saved.error));

    // Байты — ровно text.toUtf8(): ни BOM, ни transcoding-потерь.
    const QByteArray raw = readRaw(path);
    QCOMPARE(raw, text.toUtf8());
    QVERIFY(raw.startsWith(QByteArray("Привет")));

    const TextLoadResult loaded = TextFileIO::loadUtf8(path);
    QVERIFY2(loaded.ok, qPrintable(loaded.error));
    QVERIFY(loaded.error.isEmpty());
    QCOMPARE(loaded.text, text);
}

// Повторная запись в тот же путь: читается новое содержимое (коммит
// QSaveFile атомарно подменяет файл).
void TextFileIOTest::overwritesExistingFileAtomically()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("rewrite.txt"));

    QVERIFY(TextFileIO::saveUtf8(path, QStringLiteral("версия 1")).ok);
    QVERIFY(TextFileIO::saveUtf8(path, QStringLiteral("версия 2")).ok);

    QCOMPARE(readRaw(path), QByteArray("версия 2"));
    const TextLoadResult loaded = TextFileIO::loadUtf8(path);
    QVERIFY(loaded.ok);
    QCOMPARE(loaded.text, QStringLiteral("версия 2"));
}

// Файл с BOM (например, из другого редактора): BOM не становится
// символом в начале текста.
void TextFileIOTest::stripsLeadingBomOnLoad()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bom.txt"));
    QVERIFY(writeRaw(path, QByteArray("\xEF\xBB\xBF")
                                + QStringLiteral("с бом").toUtf8()));

    const TextLoadResult loaded = TextFileIO::loadUtf8(path);
    QVERIFY(loaded.ok);
    QCOMPARE(loaded.text, QStringLiteral("с бом"));
    QVERIFY(!loaded.text.startsWith(QChar(0xFEFF)));
}

// Несуществующий файл — ошибка с текстом, а не ok с пустым текстом
// (иначе «открытие» затёрло бы документ пустотой).
void TextFileIOTest::loadsMissingFileAsError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const TextLoadResult loaded = TextFileIO::loadUtf8(
        dir.filePath(QStringLiteral("нет_такого.txt")));
    QVERIFY(!loaded.ok);
    QVERIFY(!loaded.error.isEmpty());
    QVERIFY(loaded.text.isEmpty());
}

// Каталог вместо файла — ошибка чтения.
void TextFileIOTest::loadsDirectoryAsError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const TextLoadResult loaded = TextFileIO::loadUtf8(dir.path());
    QVERIFY(!loaded.ok);
    QVERIFY(!loaded.error.isEmpty());
}

// Запись в несуществующий каталог: ошибка, файл не появился
// (QSaveFile откатывает temp-файл, commit не вызывается).
void TextFileIOTest::savesToMissingDirectoryAsError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path =
        dir.filePath(QStringLiteral("нет_каталога/файл.txt"));

    const TextSaveResult saved =
        TextFileIO::saveUtf8(path, QStringLiteral("текст"));
    QVERIFY(!saved.ok);
    QVERIFY(!saved.error.isEmpty());
    QVERIFY(!QFile::exists(path));
}

// Пустой документ — валидный файл (0 байт), чтение возвращает пустой
// текст с ok (не ошибку).
void TextFileIOTest::savesEmptyText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("empty.txt"));

    QVERIFY(TextFileIO::saveUtf8(path, QString()).ok);
    QVERIFY(QFile::exists(path));
    QCOMPARE(readRaw(path), QByteArray());

    const TextLoadResult loaded = TextFileIO::loadUtf8(path);
    QVERIFY(loaded.ok);
    QVERIFY(loaded.text.isEmpty());
}

QTEST_GUILESS_MAIN(TextFileIOTest)

#include "text_file_io_test.moc"
