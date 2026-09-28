// llama_backend.cpp
#include "llama_backend.h"

#include "llama-cpp.h" // RAII-deleter: llama_model_ptr / llama_context_ptr / sampler_ptr
#include "llama.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSettings>
#include <QThread>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

Q_LOGGING_CATEGORY(llamaLog, "ptuch.llama")

namespace {

// Жёсткие пределы, чтобы не выделять безразмерный контекст и не
// генерировать бесконечно: n_ctx — в разумных границах, длина промпта
// в символах — последний рубеж (контроллер обрезает раньше).
constexpr int kMinContextSize = 256;
constexpr int kMaxContextSize = 8192;
constexpr int kMaxPromptChars = 8192;
constexpr int kMaxGeneratedTokens = 512;
// Чанк prompt processing: между llama_decode — проверка отмены.
constexpr int kPromptBatchSize = 512;

// RAII для llama_batch: освобождение на всех путях выхода.
struct BatchGuard {
    llama_batch batch {};
    bool active = false;

    explicit BatchGuard(llama_batch b)
        : batch(b)
        , active(true)
    {
    }
    ~BatchGuard()
    {
        if (active)
            llama_batch_free(batch);
    }
    BatchGuard(const BatchGuard&) = delete;
    BatchGuard& operator=(const BatchGuard&) = delete;
};

// Заполнение батча: позиции абсолютные (seq 0), logits — только у
// последнего токена последнего чанка (для следующего шага сэмплинга).
void fillBatch(llama_batch& batch, const llama_token* tokens, int count,
               int startPos, bool wantLastLogits)
{
    batch.n_tokens = count;
    for (int i = 0; i < count; ++i) {
        batch.token[i] = tokens[static_cast<std::size_t>(i)];
        batch.pos[i] = startPos + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = (wantLastLogits && i == count - 1) ? 1 : 0;
    }
}

// Кусочек токена в строку: буфер на стеке, при нехватке — временный
// вектор; указатели на временные объекты нигде не сохраняются.
void appendTokenPiece(const llama_vocab* vocab, llama_token token,
                      std::string& out)
{
    char buf[512];
    const int written = llama_token_to_piece(vocab, token, buf,
                                             static_cast<int>(sizeof(buf)),
                                             /*lstrip*/ 0, /*special*/ false);
    if (written >= 0) {
        out.append(buf, static_cast<std::size_t>(written));
        return;
    }
    // Отрицательное значение = требуемый размер буфера.
    std::vector<char> big(static_cast<std::size_t>(-written));
    const int n = llama_token_to_piece(vocab, token, big.data(),
                                       static_cast<int>(big.size()), 0, false);
    if (n > 0)
        out.append(big.data(), static_cast<std::size_t>(n));
}

} // namespace

// ---------------------------------------------------------------------------
// Поиск модели: env -> настройки -> папка models/ проекта
// ---------------------------------------------------------------------------

QString resolveModelPath()
{
    const auto existingFile = [](const QString& path) -> QString {
        return path.isEmpty() || !QFileInfo::exists(path)
            ? QString()
            : QFileInfo(path).absoluteFilePath();
    };
    const auto firstGgufIn = [](const QString& dirPath) -> QString {
        const QDir dir(dirPath);
        if (!dir.exists())
            return {};
        const QStringList ggufs = dir.entryList({QStringLiteral("*.gguf")},
                                                QDir::Files, QDir::Name);
        return ggufs.isEmpty() ? QString() : dir.absoluteFilePath(ggufs.first());
    };

    // 1) Явный override окружением.
    const QString envPath =
        existingFile(qEnvironmentVariable("PTUCH_MODEL_PATH"));
    if (!envPath.isEmpty()) {
        qCInfo(llamaLog) << "модель из PTUCH_MODEL_PATH:" << envPath;
        return envPath;
    }
    const QString envDir =
        firstGgufIn(qEnvironmentVariable("PTUCH_MODEL_DIR"));
    if (!envDir.isEmpty()) {
        qCInfo(llamaLog) << "модель из PTUCH_MODEL_DIR:" << envDir;
        return envDir;
    }

    // 2) Настройки приложения.
    QSettings settings(QStringLiteral("PtuchAI"), QStringLiteral("PtuchEditor"));
    const QString settingsPath =
        existingFile(settings.value(QStringLiteral("llama/modelPath")).toString());
    if (!settingsPath.isEmpty()) {
        qCInfo(llamaLog) << "модель из настроек:" << settingsPath;
        return settingsPath;
    }

    // 3) Папка models/ проекта: текущий каталог, каталог бинаря и
    //    проект от бинаря бандла (build/PtuchEditor.app/Contents/MacOS).
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir::current().filePath(QStringLiteral("models")),
        QDir::current().filePath(QStringLiteral("../models")),
        QDir(appDir).filePath(QStringLiteral("models")),
        QDir(appDir).filePath(QStringLiteral("../models")),
        QDir(appDir).filePath(QStringLiteral("../../../../models")),
    };
    for (const QString& candidate : candidates) {
        const QString found = firstGgufIn(candidate);
        if (!found.isEmpty()) {
            qCInfo(llamaLog) << "модель найдена в папке models:" << found;
            return found;
        }
    }

    qCInfo(llamaLog) << "GGUF-модель не найдена (настройки/окружение/models)";
    return {};
}

// ---------------------------------------------------------------------------
// LlamaBackend
// ---------------------------------------------------------------------------

struct LlamaBackend::Private {
    // RAII-освобождение (llama-cpp.h): порядок уничтожения — обратный
    // порядку объявления, т.е. context освобождается раньше model.
    llama_model_ptr model;
    llama_context_ptr context;

    // Конфигурация: пишется до старта worker-потока, дальше read-only.
    LlamaBackend* q = nullptr;
    QString modelPath;
    int contextSize = 4096;
    int gpuLayers = -1; // -1 = все слои на GPU (Metal)

    // Кросспоточные флаги: пишутся из любого потока, читает worker.
    std::atomic_bool stopRequested { false };   // стоп загрузки/генерации
    std::atomic_bool cancelRequested { false }; // отмена активного запроса
    std::atomic<quint64> activeRequestId { 0 }; // id последнего запроса
    std::atomic_bool loaded { false };

    // Только worker-поток (троттлинг прогресса до целых процентов).
    int lastProgressPercent = -1;
    // Фактический n_ctx после успешной загрузки (для повторного modelLoaded).
    int lastContextSize = 0;
};

LlamaBackend::LlamaBackend(QObject* parent)
    : ITextGenerationBackend(parent)
    , d(std::make_unique<Private>())
{
    d->q = this;
}

LlamaBackend::~LlamaBackend() = default;

void LlamaBackend::configure(const QString& modelPath, int contextSize,
                             int gpuLayers)
{
    if (d->loaded.load()) {
        qCWarning(llamaLog) << "configure() после загрузки проигнорировано";
        return;
    }
    d->modelPath = modelPath;
    d->contextSize = contextSize;
    d->gpuLayers = gpuLayers;
}

bool LlamaBackend::isModelLoaded() const
{
    return d->loaded.load();
}

void LlamaBackend::requestStop()
{
    // Атомарно и из любого потока: прерывает progress_callback при
    // загрузке и текущую генерацию. llama-вызовов здесь нет.
    d->stopRequested.store(true);
    d->cancelRequested.store(true);
}

void LlamaBackend::generate(const GenerationRequest& request)
{
    // Последний запрошенный id выигрывает: уже поставленные в очередь
    // устаревшие запросы при старте увидят несовпадение id и завершатся
    // без генерации (потребитель всё равно сверяет requestId).
    d->cancelRequested.store(false);
    d->activeRequestId.store(request.requestId);

    if (QThread::currentThread() != thread()) {
        // Вызов из чужого (UI) потока: llama-работа переносится в worker
        // queued-событием — UI не блокируется и не ждёт.
        QMetaObject::invokeMethod(
            this,
            [this, request]() { generateInWorker(request); },
            Qt::QueuedConnection);
        return;
    }
    // Поток объекта (тесты без moveToThread) — синхронно.
    generateInWorker(request);
}

void LlamaBackend::cancel(quint64 requestId)
{
    // Только атомарный флаг: безопасно из любого потока, читает его цикл
    // генерации в worker'е. Не совпало с активным id — нечего отменять.
    if (d->activeRequestId.load() == requestId)
        d->cancelRequested.store(true);
}

void LlamaBackend::loadModel()
{
    // Инициализация llama/ggml (f16-таблицы, загрузка бэкендов) — один
    // раз на процесс. llama_backend_free() освобождает только таблицы
    // квантования — достаточно на exit, особых вызовов не требует.
    static std::once_flag s_initFlag;
    std::call_once(s_initFlag, []() { llama_backend_init(); });

    if (d->loaded.load()) {
        // Модель загружается один раз: повторный вызов лишь повторяет
        // сигнал успешной загрузки (setBackend потребителя идемпотентен).
        qCInfo(llamaLog) << "loadModel: модель уже загружена — повторная"
                         << "загрузка не выполняется";
        emit modelLoaded(d->modelPath, d->lastContextSize, 0);
        return;
    }

    if (d->modelPath.isEmpty()) {
        emit modelLoadFailed(tr("Путь к GGUF-модели не задан"));
        return;
    }

    QElapsedTimer timer;
    timer.start();
    qCInfo(llamaLog) << "загрузка модели:" << d->modelPath
                     << "| gpuLayers:" << d->gpuLayers
                     << "| contextSize:" << d->contextSize;

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = d->gpuLayers;
    // Прогресс и отмена загрузки: callback приходит из worker-потока
    // (вызывается внутри llama_model_load_from_file там же).
    modelParams.progress_callback = [](float progress, void* userData) -> bool {
        auto* self = static_cast<LlamaBackend::Private*>(userData);
        if (self->stopRequested.load())
            return false; // прервать загрузку
        const int percent = static_cast<int>(progress * 100.0f);
        if (percent != self->lastProgressPercent) {
            self->lastProgressPercent = percent;
            emit self->q->progressChanged(progress);
        }
        return true;
    };
    modelParams.progress_callback_user_data = d.get();

    // Локальная QByteArray: указатель живёт до конца вызова загрузки.
    const QByteArray path = QFile::encodeName(d->modelPath);
    llama_model* rawModel =
        llama_model_load_from_file(path.constData(), modelParams);
    if (rawModel == nullptr) {
        const QString error = d->stopRequested.load()
            ? tr("Загрузка модели прервана")
            : tr("Не удалось загрузить GGUF-модель: %1").arg(d->modelPath);
        qCWarning(llamaLog) << "загрузка модели не удалась:" << error;
        emit modelLoadFailed(error);
        return;
    }
    d->model = llama_model_ptr(rawModel);

    // Ограничение размера контекста: настройка, зажатая в границы, и не
    // больше контекста обучения модели.
    llama_context_params ctxParams = llama_context_default_params();
    int nCtx = qBound(kMinContextSize, d->contextSize, kMaxContextSize);
    const int trainCtx = llama_model_n_ctx_train(rawModel);
    if (trainCtx > 0)
        nCtx = qMin(nCtx, trainCtx);
    ctxParams.n_ctx = static_cast<uint32_t>(nCtx);
    ctxParams.n_batch = kPromptBatchSize;
    const int threads = qMax(1, QThread::idealThreadCount());
    ctxParams.n_threads = threads;
    ctxParams.n_threads_batch = threads;

    llama_context* rawContext = llama_init_from_model(rawModel, ctxParams);
    if (rawContext == nullptr) {
        const QString error =
            tr("Не удалось создать контекст llama.cpp (n_ctx=%1)").arg(nCtx);
        qCWarning(llamaLog) << error;
        d->model.reset(); // освобождаем модель сразу, контекста нет
        emit modelLoadFailed(error);
        return;
    }
    d->context = llama_context_ptr(rawContext);
    d->lastContextSize = nCtx;
    d->loaded.store(true);

    const qint64 elapsedMs = timer.elapsed();
    qCInfo(llamaLog) << "модель загружена:" << d->modelPath
                     << "| время загрузки:" << elapsedMs << "мс"
                     << "| n_ctx:" << nCtx << "| потоки:" << threads;
    emit modelLoaded(d->modelPath, nCtx, elapsedMs);
}

void LlamaBackend::generateInWorker(const GenerationRequest& request)
{
    const quint64 id = request.requestId;
    qCInfo(llamaLog) << "запрос" << id << "в потоке"
                     << QThread::currentThread()->objectName();

    // Устарел ещё до старта (запрошен более новый запрос) — молча выходим:
    // активный более новый запрос сам даст ответ.
    if (d->activeRequestId.load() != id)
        return;

    if (!d->loaded.load() || !d->model || !d->context) {
        emit generationError(id, tr("Модель llama.cpp не загружена"));
        return;
    }

    // Общий таймер результата (elapsedMs) + логи по фазам.
    QElapsedTimer total;
    total.start();

    const auto cancelled = [this, id]() {
        return d->stopRequested.load() || d->cancelRequested.load()
            || d->activeRequestId.load() != id;
    };
    const auto finish = [this, &total, id](const QString& stopReason,
                                           const std::string& text) {
        GenerationResult result;
        result.requestId = id;
        // Копия в QString — указатель на локальную строку не сохраняется.
        result.generatedText = QString::fromStdString(text);
        result.stopReason = stopReason;
        result.elapsedMs = total.elapsed();
        qCInfo(llamaLog) << "запрос" << id << "завершён:"
                         << "символов:" << result.generatedText.size()
                         << "| причина:" << stopReason
                         << "| elapsedMs:" << result.elapsedMs;
        emit generationReady(result);
    };

    const llama_vocab* vocab = llama_model_get_vocab(d->model.get());
    if (vocab == nullptr) {
        emit generationError(id, tr("Словарь модели недоступен"));
        return;
    }

    // --- Промпт --------------------------------------------------------
    // Ghost-подсказка = продолжение префикса, поэтому сырой текст без
    // чат-шаблона (чат-шаблон — для chat-режима, отдельная задача).
    QByteArray promptText = request.context.toUtf8();
    if (promptText.trimmed().isEmpty()) {
        emit generationError(id, tr("Пустой контекст — генерация невозможна"));
        return;
    }
    if (promptText.size() > kMaxPromptChars) {
        qCWarning(llamaLog) << "запрос" << id << ": контекст обрезан"
                            << promptText.size() << "->" << kMaxPromptChars
                            << "символов";
        promptText = promptText.right(kMaxPromptChars);
    }
    if (!request.styleWeights.isEmpty()) {
        // Веса стилей в llama-бэкенде пока не используются (TODO:
        // отображение в prompt/penalties при подключении стилей).
        qCInfo(llamaLog) << "запрос" << id << ": styleWeights"
                         << request.styleWeights.size()
                         << "шт. — игнорируются llama-бэкендом";
    }

    // Токенизация в два прохода: сначала размер, потом наполнение.
    const int needed =
        -llama_tokenize(vocab, promptText.constData(), promptText.size(),
                        nullptr, 0, /*add_special*/ true,
                        /*parse_special*/ false);
    if (needed <= 0) {
        emit generationError(id, tr("Ошибка токенизации промпта"));
        return;
    }
    std::vector<llama_token> tokens(static_cast<std::size_t>(needed));
    const int tokenCount =
        llama_tokenize(vocab, promptText.constData(), promptText.size(),
                       tokens.data(), needed, /*add_special*/ true,
                       /*parse_special*/ false);
    if (tokenCount < 0) {
        emit generationError(id, tr("Ошибка токенизации промпта"));
        return;
    }
    tokens.resize(static_cast<std::size_t>(tokenCount));

    // Ограничение контекста: prompt + генерация + запас <= n_ctx.
    // Излишек отрезается СПЕРАВИ — ближе к курсору самое свежее.
    const int nCtx = static_cast<int>(llama_n_ctx(d->context.get()));
    const int maxTokens =
        qBound(1, request.maxTokens, kMaxGeneratedTokens);
    const int budget = nCtx - maxTokens - 16;
    if (budget < 1) {
        emit generationError(id, tr("Контекст модели слишком мал: n_ctx=%1")
                                     .arg(nCtx));
        return;
    }
    if (static_cast<int>(tokens.size()) > budget) {
        qCWarning(llamaLog) << "запрос" << id << ": промпт обрезан под n_ctx:"
                            << tokens.size() << "->" << budget << "токенов";
        tokens.erase(tokens.begin(),
                     tokens.begin() + (tokens.size() - budget));
    }
    const int nPrompt = static_cast<int>(tokens.size());
    qCInfo(llamaLog) << "запрос" << id << ": prompt" << nPrompt
                     << "токенов | maxTokens:" << maxTokens
                     << "| n_ctx:" << nCtx;

    if (cancelled()) {
        finish(QStringLiteral("cancelled"), {});
        return;
    }

    BatchGuard batch(llama_batch_init(nPrompt, /*embd*/ 0, /*n_seq_max*/ 1));

    // KV-контекст чист под новый запрос (позиции начинаются с нуля).
    llama_memory_clear(llama_get_memory(d->context.get()), /*data*/ true);

    // --- Prompt processing: чанками, с проверкой отмены между ними -----
    QElapsedTimer promptTimer;
    promptTimer.start();
    bool promptCancelled = false;
    bool promptFailed = false;
    int chunks = 0;
    for (int start = 0; start < nPrompt; start += kPromptBatchSize) {
        if (cancelled()) {
            promptCancelled = true;
            break;
        }
        const int count = qMin(kPromptBatchSize, nPrompt - start);
        const bool lastChunk = start + count == nPrompt;
        fillBatch(batch.batch, tokens.data() + start, count, start, lastChunk);
        if (llama_decode(d->context.get(), batch.batch) != 0) {
            promptFailed = true;
            break;
        }
        ++chunks;
    }
    if (promptFailed) {
        emit generationError(id, tr("Ошибка обработки промпта (llama_decode)"));
        return;
    }
    if (promptCancelled) {
        finish(QStringLiteral("cancelled"), {});
        return;
    }
    qCInfo(llamaLog) << "запрос" << id << ": prompt processing" << nPrompt
                     << "токенов за" << promptTimer.elapsed() << "мс | чанков:"
                     << chunks;

    // --- Сэмплинг: цепочка на каждый запрос, освобождение RAII ---------
    llama_sampler_ptr sampler(
        llama_sampler_chain_init(llama_sampler_chain_default_params()));
    if (!sampler) {
        emit generationError(id, tr("Не удалось создать цепочку сэмплера"));
        return;
    }
    const float topP =
        request.topP > 0.0 ? static_cast<float>(request.topP) : 0.9f;
    llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_p(topP, 1));
    const double temperature =
        qBound(0.1, request.temperature, 2.0);
    llama_sampler_chain_add(sampler.get(),
                            llama_sampler_init_temp(static_cast<float>(temperature)));
    // Seed от id: воспроизводимо внутри запроса, меняется между
    // запросами/альтернативами (Shift+Tab даёт новый id).
    llama_sampler_chain_add(
        sampler.get(), llama_sampler_init_dist(static_cast<uint32_t>(id)));

    // --- Генерация ------------------------------------------------------
    QElapsedTimer genTimer;
    genTimer.start();
    std::string generated;
    generated.reserve(256);
    int generatedTokens = 0;
    QString stopReason = QStringLiteral("length"); // цикл отработал по maxTokens

    for (int step = 0; step < maxTokens; ++step) {
        if (cancelled()) {
            stopReason = QStringLiteral("cancelled");
            break;
        }

        const llama_token token =
            llama_sampler_sample(sampler.get(), d->context.get(), -1);
        // llama_sampler_sample уже делает llama_sampler_accept внутри —
        // повторный accept не нужен.

        if (llama_vocab_is_eog(vocab, token)) {
            stopReason = QStringLiteral("stop");
            break;
        }
        appendTokenPiece(vocab, token, generated);
        ++generatedTokens;

        // Декодируем выбранный токен — логиты для следующего шага.
        const llama_token decoded[1] = { token };
        fillBatch(batch.batch, decoded, 1, nPrompt + step, /*logits*/ true);
        if (llama_decode(d->context.get(), batch.batch) != 0) {
            emit generationError(id,
                                 tr("Ошибка генерации (llama_decode)"));
            return;
        }
    }

    qCInfo(llamaLog) << "запрос" << id << ": генерация" << generatedTokens
                     << "токенов за" << genTimer.elapsed() << "мс";

    finish(stopReason, generated);
}
