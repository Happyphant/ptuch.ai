// llama_backend.h
#pragma once

#include "backend/text_generation_backend.h"

#include <QString>
#include <memory>

// Поиск GGUF-модели (первое совпадение):
//   1) env PTUCH_MODEL_PATH (файл) или PTUCH_MODEL_DIR (папка);
//   2) настройки QSettings (PtuchAI/PtuchEditor, ключ llama/modelPath);
//   3) сканирование папок models/: текущий каталог, каталог бинаря,
//      каталог проекта от бинаря бандла.
// Пустая строка = модель не найдена (потребитель остаётся на mock).
QString resolveModelPath();

// LlamaBackend — реализация ITextGenerationBackend поверх llama.cpp.
//
// Потоки: объект создаётся в UI-потоке и сразу moveToThread(worker).
// ВСЯ llama-активность (загрузка GGUF, llama_decode, sampling) выполняется
// только в worker-потоке:
//   - generate() из любого потока переносит работу queued-событием;
//   - cancel()/requestStop() — только атомарные флаги, их читает цикл
//     генерации в worker'е (llama-вызовов в вызывающем потоке нет);
//   - UI-поток никогда не вызывает llama_decode/sampling и не спит.
//
// Владение: llama_model/llama_context — RAII (unique_ptr с deleter из
// llama-cpp.h) внутри pimpl; деструктор выполняется в worker-потоке
// (deleteLater по finished) и освобождает context, затем model.
// Один llama_context на объект; запросы обрабатываются строго
// последовательно — параллельного доступа к контексту нет.
//
// Жизненный цикл модели: configure() до старта потока, loadModel() —
// один раз (идемпотентен); при отсутствии/ошибке модели потребитель
// остаётся на mock-бэкенде (см. MainWindow::setupLlamaBackend).
class LlamaBackend final : public ITextGenerationBackend
{
    Q_OBJECT

public:
    explicit LlamaBackend(QObject* parent = nullptr);
    ~LlamaBackend() override;

    // Конфигурация до loadModel() (вызывается до moveToThread/старта
    // потока; после загрузки игнорируется — модель загружается один раз).
    void configure(const QString& modelPath, int contextSize, int gpuLayers);

    // ITextGenerationBackend: безопасны из любого потока.
    void generate(const GenerationRequest& request) override;
    void cancel(quint64 requestId) override;
    // Снимок диагностики (потокобезопасная копия под мьютексом):
    // имя модели, n_ctx, GPU-слои, метрики последнего успешного
    // запроса и последняя ошибка.
    BackendDiagnostics diagnostics() const override;

    // Прервать загрузку модели и текущую генерацию из любого потока
    // (атомарные флаги). Вызывается при закрытии приложения перед
    // thread->quit().
    void requestStop();

    bool isModelLoaded() const;

public slots:
    // Загрузка GGUF. Выполняется в потоке объекта (queued-вызов) либо
    // синхронно в текущем потоке — в тестах. Идемпотентна: повторный
    // вызов при уже загруженной модели просто повторяет modelLoaded.
    void loadModel();

signals:
    // Успех: путь, фактический n_ctx, время загрузки (мс).
    void modelLoaded(const QString& modelPath, int contextSize,
                     qint64 elapsedMs);
    void modelLoadFailed(const QString& error);
    // Прогресс загрузки 0.0..1.0 (троттлинг до целых процентов).
    void progressChanged(float progress);

private:
    // Вся llama-работа (загрузка/декод/sampling) — только здесь,
    // в потоке объекта.
    void generateInWorker(const GenerationRequest& request);
    // Последняя ошибка диагностики (пустая строка = очистить — успех
    // разрешает ошибку). Пишется из worker'а, читается из UI — под
    // мьютексом снимка.
    void setDiagnosticError(const QString& error);

    struct Private;
    std::unique_ptr<Private> d;
};
