#pragma once

#include "async_pipeline.h"
#include <QMutex>
#include <functional>

class SerialConflatedAsyncPipeline final : public IAsyncPipeline, private IWorkObserver
{
public:
    bool initialize(IWorkRuntime *runtime,
                    IWorkProcessor *processor,
                    IFramePublisher *publisher,
                    IFrameWriter *frameWriter,
                    IWorkScheduler *scheduler,
                    QString *error) override;

    void submit(const WorkEnvelope &work) override;
    void pump() override;
    void onPublicationCapacityAvailable() override;
    void setStateChangedCallback(std::function<void(RequestId, WorkState)> callback) override;
    void setProgressCallback(std::function<void(quint64, int, bool)> callback) override;
    void setMessageCallback(std::function<void(RequestId, const QString &)> callback) override;
    void setResultReadyCallback(std::function<void(const JobResult &)> callback) override;
    void setErrorCallback(std::function<void(const QString &)> callback) override;
    void shutdown() override;

private:
    void onStateChanged(RequestId requestId, WorkState state) override;
    void onProgress(RequestId requestId, int progress, bool isFinal) override;
    void onMessage(RequestId requestId, const QString &message) override;
    bool pumpStep(QString *error);
    bool shouldDeliverResult(const WorkEnvelope &completedWork) const;

    IWorkRuntime *m_runtime = nullptr;
    IWorkProcessor *m_processor = nullptr;
    IFramePublisher *m_publisher = nullptr;
    IWorkScheduler *m_scheduler = nullptr;
    bool m_initialized = false;
    bool m_pumpActive = false;
    bool m_pumpRequested = false;
    mutable QMutex m_mutex;
    std::function<void(RequestId, WorkState)> m_stateChangedCallback;
    std::function<void(quint64, int, bool)> m_progressCallback;
    std::function<void(RequestId, const QString &)> m_messageCallback;
    std::function<void(const JobResult &)> m_resultReadyCallback;
    std::function<void(const QString &)> m_errorCallback;
};
