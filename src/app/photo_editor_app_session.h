#pragma once

#include "framework/execution/async_lane.h"
#include "framework/execution/execution_common.h"
#include "framework/execution/runtime_invoker.h"
#include "framework/platform/texture_types.h"
#include "image_effect_types.h"
#include "photo_editor/photo_editor_result_types.h"
#include "photo_editor/photo_editor_gpu_session.h"
#include "photo_editor/photo_editor_render_args.h"

#include <QImage>
#include <QObject>
#include <QSize>

#include <memory>

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

    class PhotoEditorGpuPreviewArgsMerger final : public IWaitingMerger<PhotoEditorGpuPreviewArgs>
    {
    public:
        bool canMerge(const PhotoEditorGpuPreviewArgs &waiting,
                      const PhotoEditorGpuPreviewArgs &incoming) const override;
        PhotoEditorGpuPreviewArgs merge(const PhotoEditorGpuPreviewArgs &waiting,
                                        const PhotoEditorGpuPreviewArgs &incoming) const override;
    };

    PhotoEditorSourceSnapshot currentSourceSnapshot() const;
    PhotoEditorGpuPreviewArgs buildGpuPreviewArgs() const;
    PhotoEditorCpuPreviewArgs buildCpuPreviewArgs() const;

    void emitImageSelection();
    void submitGpuPreview();
    void runCpuPreviewSync();
    void handleGpuPreviewCompleted(TaskId taskId, ExecutionOutcome<RawGpuTextureResult> outcome);

    std::unique_ptr<ImageCatalogState> m_catalog;
    IPlatformBackend *m_backend = nullptr;
    std::unique_ptr<QtRuntimeHost> m_runtimeHost;
    std::unique_ptr<RuntimeInvoker> m_invoker;
    std::unique_ptr<PhotoEditorGpuSession> m_gpuSession;
    std::unique_ptr<AsyncLane<PhotoEditorGpuPreviewArgs, RawGpuTextureResult>> m_gpuPreviewLane;
    ImageEffectParameters m_effectParameters;
    QSize m_outputSize;
    quint64 m_outputRevision = 0;
    bool m_initialized = false;
    bool m_shuttingDown = false;
};
