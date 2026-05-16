#pragma once

#include "framework/execution/request_dispatcher.h"
#include "framework/execution/request_result_sink.h"
#include "framework/platform/texture_types.h"
#include "image_effect_types.h"

#include <QImage>
#include <QObject>
#include <QSize>

#include <memory>

class IPlatformBackend;
class IRuntime;
struct RawGpuTextureResult;

class PhotoEditorAppSession final : public QObject, public IRequestResultSink
{
    Q_OBJECT

public:
    explicit PhotoEditorAppSession(QObject *parent = nullptr);
    ~PhotoEditorAppSession() override;

public slots:
    bool initialize(IPlatformBackend *backend, QSize outputSize);
    void setOutputSize(QSize size);
    void setEffectParameters(const ImageEffectParameters &parameters);
    void loadImageDirectory(const QString &directoryPath);
    void selectNextImage();
    void selectPreviousImage();
    void requestRender();
    void requestCpuPreview();
    void onTextureConsumed();
    void shutdown();

signals:
    void textureReady(const TextureTicket &ticket);
    void initializationFailed(const QString &reason);
    void imageDirectoryLoadFinished(bool loaded,
                                    const QString &errorMessage,
                                    int currentIndex,
                                    int count,
                                    const QString &displayName,
                                    QSize imageSize);
    void imageSelectionChanged(int currentIndex, int count, const QString &displayName, QSize imageSize);
    void cpuPreviewReady(const QImage &image, const QString &description);

private:
    enum class GpuPublishDisposition
    {
        Published,
        RetryLater,
        Failed
    };

    struct ImageCatalogState;

    void onResultReady(const ExecutionResult &result, IExecutionContext &context) override;
    void onRequestFailed(RequestId requestId, const QString &error) override;

    void emitImageSelection();
    void submitGpuRequest();
    void submitCpuRequest();
    GpuPublishDisposition tryPublishGpuResult(const RawGpuTextureResult &gpuResult, IRuntime *sourceRuntime);
    ExecutionRequest buildGpuRequest() const;
    ExecutionRequest buildCpuRequest() const;

    std::unique_ptr<ImageCatalogState> m_catalog;
    IPlatformBackend *m_backend = nullptr;
    std::unique_ptr<IRuntime> m_runtime;
    std::unique_ptr<RequestDispatcher> m_dispatcher;
    ImageEffectParameters m_effectParameters;
    QSize m_outputSize;
    RequestId m_requestSequence = 0;
    bool m_initialized = false;
    bool m_shuttingDown = false;
    bool m_hasPendingGpuPublish = false;
    RawGpuTextureResult m_pendingGpuPublish;
};
