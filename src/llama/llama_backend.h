// llama_backend.h
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

struct LlamaGenerationParams {
    int maxTokens = 128;
    int contextSize = 4096;
    float temperature = 0.7f;
    float topP = 0.9f;
    int seed = -1;
};

class LlamaBackend final : public QObject {
    Q_OBJECT

public:
    explicit LlamaBackend(QObject* parent = nullptr);
    virtual ~LlamaBackend() override;

    bool loadModel(const QString& modelPath,
                   int contextSize,
                   int gpuLayers);

    void generate(const QString& prompt,
                  const LlamaGenerationParams& params);

public slots:
    void cancel();

signals:
    void tokenGenerated(const QString& token);
    void generationFinished();
    void errorOccurred(const QString& message);
    void progressChanged(float progress);

private:
    class Private;
    std::unique_ptr<Private> d;
};
