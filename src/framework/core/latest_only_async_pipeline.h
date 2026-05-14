#pragma once

#include "async_pipeline.h"
#include "simple_artifact_builder.h"

#include <functional>
#include <QMutex>

class LatestOnlyAsyncPipeline final : public IAsyncPipeline, private IWorkObserver
{
public:
    bool initialize(IWorkRuntime *runtime,
                    IWorkProcessor *processor,
                    IArtifactPublisher *publisher,
                    IWorkScheduler *scheduler,
                    QString *error) override;

    void submit(const WorkEnvelope &work) override;
    void pump() override;
    void onPublicationCapacityAvailable() override;
    void setProgressCallback(std::function<void(quint64, int, bool)> callback) override;
    void setFrameReadyCallback(std::function<void(const PublicationTicket &)> callback) override;
    void setErrorCallback(std::function<void(const QString &)> callback) override;
    void shutdown() override;

private:
    void onStateChanged(quint64 workId, WorkState state) override;
    void onProgress(quint64 workId, int progress, bool isFinal) override;
    void onMessage(quint64 workId, const QString &message) override;
    bool pumpStep(QString *error);

    IWorkRuntime *m_runtime = nullptr;
    IWorkProcessor *m_processor = nullptr;
    IArtifactPublisher *m_publisher = nullptr;
    IWorkScheduler *m_scheduler = nullptr;
    bool m_initialized = false;
    bool m_processing = false;
    bool m_publishing = false;
    bool m_pumpActive = false;
    bool m_pumpRequested = false;
    bool m_hasPendingPublication = false;
    mutable QMutex m_mutex;
    SimpleArtifactBuilder m_builder;
    WorkEnvelope m_activeWork;
    WorkEnvelope m_pendingWork;
    ArtifactSnapshot m_pendingArtifact;
    std::function<void(quint64, int, bool)> m_progressCallback;
    std::function<void(const PublicationTicket &)> m_frameReadyCallback;
    std::function<void(const QString &)> m_errorCallback;
};
