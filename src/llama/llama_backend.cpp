
#include "llama_backend.h"

#include "llama.h"
#include "llama-cpp.h"

struct LlamaBackend::Private {
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    llama_sampler* sampler = nullptr;

    std::atomic_bool cancelRequested = false;
};

LlamaBackend::LlamaBackend(QObject *parent)
{
}

LlamaBackend::~LlamaBackend()
{
}

bool LlamaBackend::loadModel(const QString &modelPath,
                             int contextSize,
                             int gpuLayers)
{
    llama_model_params modelParams =
        llama_model_default_params();

    modelParams.n_gpu_layers = gpuLayers;

    const QByteArray path = modelPath.toUtf8();

    d->model = llama_model_load_from_file(
        path.constData(),
        modelParams
    );

    if (!d->model) {
        emit errorOccurred("Не удалось загрузить GGUF-модель");
        return false;
    }

    llama_context_params contextParams =
        llama_context_default_params();

    contextParams.n_ctx = contextSize;
    contextParams.n_batch = 512;

    d->context = llama_init_from_model(
        d->model,
        contextParams
    );

    if (!d->context) {
        emit errorOccurred("Не удалось создать llama context");
        return false;
    }

    return true;
}

void LlamaBackend::generate(const QString& prompt,
                            const LlamaGenerationParams& params)
{
    d->cancelRequested = false;

    const QByteArray text = prompt.toUtf8();

    std::vector<llama_token> tokens(
        text.size() + 512
    );

    const int tokenCount = llama_tokenize(
        llama_model_get_vocab(d->model),
        text.constData(),
        text.size(),
        tokens.data(),
        static_cast<int>(tokens.size()),
        true,
        true
    );

    if (tokenCount < 0) {
        emit errorOccurred("Ошибка токенизации");
        return;
    }

    tokens.resize(tokenCount);

    // Далее:
    // 1. llama_decode() для prompt-токенов
    // 2. получение logits
    // 3. sampler chain
    // 4. llama_decode() для каждого нового токена
    // 5. emit tokenGenerated(...)
}

void LlamaBackend::cancel()
{
    d->cancelRequested = true;
    // TODO:
}

