#include <QApplication>

#include "UI/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Имя организации/приложения — для QSettings: путь к GGUF-модели
    // хранится в ключе llama/modelPath (см. resolveModelPath()).
    QCoreApplication::setOrganizationName(QStringLiteral("PtuchAI"));
    QCoreApplication::setApplicationName(QStringLiteral("PtuchEditor"));

    // Потоки backend'ов (mock + llama workers) заводит сам MainWindow:
    // он же корректно останавливает их в closeEvent/деструкторе.
    MainWindow window;
    window.show();

    return app.exec();
}
