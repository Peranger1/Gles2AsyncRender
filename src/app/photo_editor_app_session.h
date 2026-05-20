#pragma once

#include "framework/execution/async_lane.h"
#include "framework/execution/execution_common.h"
#include "framework/platform/texture_types.h"
#include "image_effect_types.h"
#include "photo_editor/photo_editor_result_types.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <functional>
#include <memory>
#include <utility>

class IPlatformBackend;
class QtRuntimeHost;

class PhotoEditorAppSession final : public QObject
{
    Q_OBJECT

public:
    explicit PhotoEditorAppSession(QObject *parent = nullptr);
    ~PhotoEditorAppSession() override;

public slots:
    bool initialize(IPlatformBackend *backend, QSize outputSize);
    void setOutputSize(QSize size, quint64 outputRevision);
    void setEffectParameters(const ImageEffectParameters &parameters);
    void loadImageDirectory(const QString &directoryPath);
    void selectNextImage();
    void selectPreviousImage();
    void requestRender();
    void requestCpuPreview();
    void shutdown();

signals:
    void initializationFailed(const QString &reason);
    void requestWarning(const QString &reason);
    void imageDirectoryLoadFinished(bool loaded,
                                    const QString &errorMessage,
                                    int currentIndex,
                                    int count,
                                    const QString &displayName,
                                    QSize imageSize);
    void imageSelectionChanged(int currentIndex, int count, const QString &displayName, QSize imageSize);
    void cpuPreviewReady(const QImage &image, const QString &description);

private:
    struct ImageCatalogState;
    struct GpuPreviewRequest final
    {
        QString sourceKey;
        quint64 sourceImageCacheKey = 0;
        std::shared_ptr<const QImage> sourceImage;
        ImageEffectParameters parameters;
        QSize outputSize;
    };

    struct GpuProcessBridge;
    struct ReplaceGpuPreviewForSameSource final
    {
        bool canMerge(const GpuPreviewRequest &waiting, const GpuPreviewRequest &incoming) const
        {
            return waiting.sourceKey == incoming.sourceKey;
        }

        GpuPreviewRequest merge(const GpuPreviewRequest &, GpuPreviewRequest incoming) const
        {
            return incoming;
        }
    };

    using GpuPreviewLane = AsyncLane<GpuPreviewRequest,
                                     RawGpuTextureResult,
                                     execution::MergeWhileBusyQueue<3>,
                                     execution::DeliverEveryStartedResult,
                                     ReplaceGpuPreviewForSameSource>;

    void emitImageSelection();
    void submitGpuPreview();
    void runCpuPreviewSync();
    void destroyGpuEditor();
    ExecutionOutcome<void> ensurePhotoEditorInitialized(IRuntime *runtime);
    ExecutionOutcome<void> ensureGpuEditor(IRuntime *runtime,
                                           const std::shared_ptr<const QImage> &sourceImage,
                                           const QString &sourceKey,
                                           quint64 sourceImageCacheKey,
                                           QSize outputSize);
    void runGpuPreviewStep(const TaskContext &context,
                           const GpuPreviewRequest &request,
                           GpuPreviewLane::Done done);
    ExecutionOutcome<RawGpuTextureResult> collectGpuRenderResult(IRuntime *runtime, void *handle);
    void handleGpuPreviewCompleted(TaskId taskId, ExecutionOutcome<RawGpuTextureResult> outcome);
    static void onGpuProcessProgress(int progress, bool isEnd, void *userData);

    std::unique_ptr<ImageCatalogState> m_catalog;
    IPlatformBackend *m_backend = nullptr;
    std::unique_ptr<QtRuntimeHost> m_runtimeHost;
    std::unique_ptr<GpuPreviewLane> m_gpuPreviewLane;
    void *m_gpuEditorHandle = nullptr;
    bool m_photoEditorInitialized = false;
    QString m_gpuSourceKey;
    quint64 m_gpuSourceImageCacheKey = 0;
    std::shared_ptr<const QImage> m_gpuSourceImage;
    std::shared_ptr<GpuProcessBridge> m_activeGpuBridge;
    ImageEffectParameters m_effectParameters;
    QSize m_outputSize;
    quint64 m_outputRevision = 0;
    bool m_initialized = false;
    bool m_shuttingDown = false;
};
