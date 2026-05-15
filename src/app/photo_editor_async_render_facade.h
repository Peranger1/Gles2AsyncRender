#pragma once

#include "framework/core/work_types.h"
#include "image_effect_types.h"

#include <QImage>
#include <QObject>
#include <QMutex>
#include <QSize>
#include <memory>

class AsyncTaskFacade;
class IPlatformRenderBackend;
struct FrameTicket;

class PhotoEditorAsyncRenderFacade final : public QObject
{
    Q_OBJECT

public:
    explicit PhotoEditorAsyncRenderFacade(QObject *parent = nullptr);
    ~PhotoEditorAsyncRenderFacade() override;

public slots:
    bool initialize(IPlatformRenderBackend *backend, QSize outputSize);
    void setOutputSize(QSize size);
    void setEffectParameters(const ImageEffectParameters &parameters);
    void loadImageDirectory(const QString &directoryPath);
    void selectNextImage();
    void selectPreviousImage();
    void requestRender();
    void requestCpuPreview();
    void onPublicationCapacityAvailable();
    void shutdown();

signals:
    void jobResultReady(const JobResult &result);
    void frameReady(const FrameTicket &ticket);
    void initializationFailed(const QString &reason);
    void imageDirectoryLoadFinished(bool loaded,
                                    const QString &errorMessage,
                                    int currentIndex,
                                    int count,
                                    const QString &displayName,
                                    QSize imageSize);
    void imageSelectionChanged(int currentIndex, int count, const QString &displayName, QSize imageSize);
    void processingProgressChanged(int progress);
    void cpuPreviewReady(const QImage &image, const QString &description);

private:
    struct ImageCatalogState;

    void handleStateChanged(RequestId requestId, WorkState state);
    void handleProgress(RequestId requestId, int progress, bool isFinal);
    void handleMessage(RequestId requestId, const QString &message);
    void handleJobResult(const JobResult &result);
    void handlePipelineError(const QString &reason);
    void emitImageSelection();
    void submitLatestRequest();
    void submitCpuPreviewRequest();
    WorkEnvelope buildLatestWork() const;
    WorkEnvelope buildCpuPreviewWork() const;

    std::unique_ptr<ImageCatalogState> m_catalog;
    IPlatformRenderBackend *m_backend = nullptr;
    std::unique_ptr<AsyncTaskFacade> m_taskFacade;
    std::unique_ptr<AsyncTaskFacade> m_cpuPreviewTaskFacade;
    ImageEffectParameters m_effectParameters;
    QSize m_outputSize;
    bool m_initialized = false;
    bool m_shuttingDown = false;
    mutable quint64 m_requestSequence = 0;
    mutable QMutex m_stateMutex;
};
