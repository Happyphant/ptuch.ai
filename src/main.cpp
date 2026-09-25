#include <QApplication>
#include <QThread>

#include "llama/llama_backend.h"
#include "suggestion/suggestion_controller.h"
#include "UI/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    auto* thread = new QThread();
    auto* backend = new LlamaBackend;

    backend->moveToThread(thread);

    QObject::connect(thread, &QThread::finished,
            backend, &QObject::deleteLater);

    thread->start();


    MainWindow window;

    /*
    QObject::connect(this, &SuggestionController::requestGeneration,
            backend, &LlamaBackend::generate,
            Qt::QueuedConnection);

    QObject::connect(backend, &LlamaBackend::tokenGenerated,
            this, &SuggestionController::appendSuggestion);
    */

    window.show();

    return app.exec();
}
