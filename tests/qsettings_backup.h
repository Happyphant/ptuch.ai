// qsettings_backup.h
#pragma once

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// Защита пользовательских QSettings от АВАРИЙНЫХ падений тестов.
//
// GhostSuggestionTest и SettingsTest снимают ключи приложения в
// память и возвращают их в cleanup/cleanupTestCase. Но QVERIFY и
// QTEST_ASSERT при нарушении фатальны: SIGABRT убивает процесс до
// cleanup — снимок в памяти теряется вместе с процессом и
// пользовательские настройки стираются безвозвратно.
//
// Решение: тот же снимок дублируется в ФАЙЛЕ.
//  - файл существует => прошлый прогон не дожил до восстановления:
//    restoreIfCrashed() (первым шагом старта) дословно возвращает
//    ключи из файла и удаляет его — настройки самовосстанавливаются
//    при СЛЕДУЮЩЕМ запуске тестов;
//  - обычный путь не меняется: clear() после штатного восстановления
//    убирает файл.
//
// Файл общий для всех тестовых бинарей (как и сам QSettings
// PtuchAI/PtuchEditor): в последовательном ctest гонки нет;
// параллельный ctest и так небезопасен для общего файла настроек.
namespace QSettingsBackup {

// Организация/приложение продублированы (как в AppSettings):
// файл настроек один, независимо от имени тестового бинаря.
inline QString backupFilePath()
{
    return QDir::temp().filePath(
        QStringLiteral("ptuch_editor_qsettings_backup.ini"));
}

// Первый шаг старта (ДО снимка): если файл остался от упавшего
// прогона — вернуть его ключи приложению и удалить файл.
// isOurKey — предикат «ключ принадлежит приложению»: чужие ключи
// файла (fallback NSGlobalDomain) не трогаем.
template <typename Pred>
void restoreIfCrashed(Pred isOurKey)
{
    const QString path = backupFilePath();
    if (!QFile::exists(path))
        return;

    QSettings backup(path, QSettings::IniFormat);
    QSettings app(QStringLiteral("PtuchAI"),
                  QStringLiteral("PtuchEditor"));

    // Эталон — файл снимка: сначала убираем текущие наши ключи
    // (в т.ч. мусор упавшего прогона), потом дословно возвращаем.
    const QStringList currentKeys = app.allKeys();
    for (const QString& key : currentKeys) {
        if (isOurKey(key))
            app.remove(key);
    }
    const QStringList savedKeys = backup.allKeys();
    for (const QString& key : savedKeys) {
        if (isOurKey(key))
            app.setValue(key, backup.value(key));
    }
    app.sync();
    QFile::remove(path);
}

// Второй шаг старта: продублировать памятный снимок в файл.
// Значения пишутся как есть; INI-формат при чтении возвращает
// int/double/bool/QString (строковые «48», «0.3», «true» приводятся
// QSettings обратно к типу) — для ключей приложения этого хватает.
inline void save(const QVariantMap& snapshot)
{
    QSettings backup(backupFilePath(), QSettings::IniFormat);
    backup.clear();
    for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it)
        backup.setValue(it.key(), it.value());
    backup.sync();
}

// Штатное завершение: снимок восстановлен из памяти — файл не нужен.
inline void clear()
{
    QFile::remove(backupFilePath());
}

} // namespace QSettingsBackup
