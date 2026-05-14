#pragma once

#include "framework/core/work_types.h"
#include "image_effect_types.h"

#include <QObject>
#include <QMutex>
#include <QSize>
#include <memory>

class AngleStandaloneRuntime;
class D3D11FramePublisher;
class IAsyncPipeline;
class ISharedFrameSlotPool;
class LatestOnlyWorkScheduler;
class PhotoEditorWorkProcessor;
struct PublicationTicket;

class PhotoEditorAsyncRenderFacade final : public QObject
{
    Q_OBJECT

public:
    explicit PhotoEditorAsyncRenderFacade(QObject *parent = nullptr);
    ~PhotoEditorAsyncRenderFacade() override;

public slots:
    bool initialize(ISharedFrameSlotPool *slotPool, QSize outputSize);
    void setOutputSize(QSize size);
    void setEffectParameters(const ImageEffectParameters &parameters);
    void loadImageDirectory(const QString &directoryPath);
    void selectNextImage();
    void selectPreviousImage();
    void requestRender();
    void onPublicationCapacityAvailable();
    void shutdown();

signals:
    void frameReady(int slotIndex, quint64 generation, QSize size, quint64 frameIndex);
    void initializationFailed(const QString &reason);
    void imageDirectoryLoadFinished(bool loaded,
                                    const QString &errorMessage,
                                    int currentIndex,
                                    int count,
                                    const QString &displayName,
                                    QSize imageSize);
    void imageSelectionChanged(int currentIndex, int count, const QString &displayName, QSize imageSize);
    void processingProgressChanged(int progress);

private:
    struct ImageCatalogState;

    void handleProgress(quint64 workId, int progress, bool isFinal);
    void handleFrameReady(const PublicationTicket &ticket);
    void handlePipelineError(const QString &reason);
    void emitImageSelection();
    void submitLatestRequest();
    WorkEnvelope buildLatestWork() const;

    std::unique_ptr<ImageCatalogState> m_catalog;
    std::unique_ptr<AngleStandaloneRuntime> m_runtime;
    std::unique_ptr<PhotoEditorWorkProcessor> m_processor;
    std::unique_ptr<D3D11FramePublisher> m_publisher;
    std::unique_ptr<LatestOnlyWorkScheduler> m_scheduler;
    std::unique_ptr<IAsyncPipeline> m_pipeline;
    ISharedFrameSlotPool *m_slotPool = nullptr;
    ImageEffectParameters m_effectParameters;
    QSize m_outputSize;
    bool m_initialized = false;
    bool m_shuttingDown = false;
    mutable quint64 m_requestSequence = 0;
    mutable QMutex m_stateMutex;
};
