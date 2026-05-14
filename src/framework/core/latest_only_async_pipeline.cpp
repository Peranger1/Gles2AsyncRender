#include "latest_only_async_pipeline.h"

#include <QMutexLocker>

bool LatestOnlyAsyncPipeline::initialize(IWorkRuntime *runtime,
                                         IWorkProcessor *processor,
                                         IArtifactPublisher *publisher,
                                         IWorkScheduler *scheduler,
                                         QString *error)
{
    if (runtime == nullptr || processor == nullptr || publisher == nullptr || scheduler == nullptr) {
        if (error) {
            *error = QStringLiteral("LatestOnlyAsyncPipeline requires runtime, processor, publisher, and scheduler.");
        }
        return false;
    }

    m_runtime = runtime;
    m_processor = processor;
    m_publisher = publisher;
    m_scheduler = scheduler;

    if (!m_runtime->initialize(error)) {
        shutdown();
        return false;
    }

    const bool processorReady = m_processor->initialize(*m_runtime, error);
    bool publisherReady = false;
    if (processorReady) {
        if (!m_runtime->enter(error)) {
            shutdown();
            return false;
        }

        publisherReady = m_publisher->initialize(*m_runtime, error);
        m_runtime->leave();
    }

    if (!processorReady || !publisherReady) {
        shutdown();
        return false;
    }

    m_processor->setWakeCallback([this]() {
        pump();
    });
    m_initialized = true;
    return true;
}

void LatestOnlyAsyncPipeline::submit(const WorkEnvelope &work)
{
    if (!m_initialized || m_scheduler == nullptr) {
        return;
    }

    m_scheduler->submit(work);
    pump();
}

void LatestOnlyAsyncPipeline::pump()
{
    QString error;
    for (;;) {
        {
            QMutexLocker locker(&m_mutex);
            if (m_pumpActive) {
                m_pumpRequested = true;
                return;
            }
            m_pumpActive = true;
            m_pumpRequested = false;
        }

        bool shouldContinue = false;
        do {
            shouldContinue = pumpStep(&error);
            if (!error.isEmpty()) {
                break;
            }

            QMutexLocker locker(&m_mutex);
            if (m_pumpRequested) {
                m_pumpRequested = false;
                shouldContinue = true;
            }
        } while (shouldContinue);

        bool rerun = false;
        {
            QMutexLocker locker(&m_mutex);
            rerun = m_pumpRequested;
            m_pumpRequested = false;
            m_pumpActive = false;
        }

        if (error.isEmpty() && rerun) {
            continue;
        }
        break;
    }

    if (!error.isEmpty()) {
        std::function<void(const QString &)> callback;
        {
            QMutexLocker locker(&m_mutex);
            callback = m_errorCallback;
        }
        if (callback) {
            callback(error);
        }
    }
}

void LatestOnlyAsyncPipeline::onPublicationCapacityAvailable()
{
    if (!m_initialized) {
        return;
    }

    pump();
}

void LatestOnlyAsyncPipeline::setProgressCallback(std::function<void(quint64, int, bool)> callback)
{
    QMutexLocker locker(&m_mutex);
    m_progressCallback = std::move(callback);
}

void LatestOnlyAsyncPipeline::setFrameReadyCallback(std::function<void(const PublicationTicket &)> callback)
{
    QMutexLocker locker(&m_mutex);
    m_frameReadyCallback = std::move(callback);
}

void LatestOnlyAsyncPipeline::setErrorCallback(std::function<void(const QString &)> callback)
{
    QMutexLocker locker(&m_mutex);
    m_errorCallback = std::move(callback);
}

void LatestOnlyAsyncPipeline::shutdown()
{
    QMutexLocker locker(&m_mutex);

    if (m_scheduler != nullptr) {
        m_scheduler->clear();
    }
    if (m_publisher != nullptr) {
        if (m_runtime != nullptr && m_runtime->enter(nullptr)) {
            m_publisher->shutdown();
            m_runtime->leave();
        } else {
            m_publisher->shutdown();
        }
    }
    if (m_processor != nullptr) {
        m_processor->shutdown();
    }
    if (m_runtime != nullptr) {
        m_runtime->shutdown();
    }

    m_runtime = nullptr;
    m_processor = nullptr;
    m_publisher = nullptr;
    m_scheduler = nullptr;
    m_initialized = false;
    m_processing = false;
    m_publishing = false;
    m_pumpActive = false;
    m_pumpRequested = false;
    m_hasPendingPublication = false;
    m_activeWork = {};
    m_pendingWork = {};
    m_pendingArtifact = {};
    m_builder.reset();
    m_progressCallback = {};
    m_frameReadyCallback = {};
    m_errorCallback = {};
}

void LatestOnlyAsyncPipeline::onStateChanged(quint64, WorkState)
{
}

void LatestOnlyAsyncPipeline::onProgress(quint64 workId, int progress, bool isFinal)
{
    std::function<void(quint64, int, bool)> callback;
    {
        QMutexLocker locker(&m_mutex);
        callback = m_progressCallback;
    }
    if (callback) {
        callback(workId, progress, isFinal);
    }
}

void LatestOnlyAsyncPipeline::onMessage(quint64, const QString &)
{
}

bool LatestOnlyAsyncPipeline::pumpStep(QString *error)
{
    std::function<void(const PublicationTicket &)> frameReadyCallback;
    WorkEnvelope work;
    ArtifactSnapshot artifact;
    bool publishOnly = false;
    bool collectPhase = false;
    bool startPhase = false;
    bool runtimeEntered = false;

    {
        QMutexLocker locker(&m_mutex);
        if (!m_initialized || m_runtime == nullptr || m_processor == nullptr || m_publisher == nullptr
            || m_scheduler == nullptr || m_publishing) {
            return false;
        }

        frameReadyCallback = m_frameReadyCallback;

        if (m_hasPendingPublication) {
            publishOnly = true;
            work = m_pendingWork;
            artifact = m_pendingArtifact;
            m_publishing = true;
        } else if (m_processing) {
            collectPhase = true;
            work = m_activeWork;
        } else {
            if (!m_scheduler->takeNext(&work)) {
                return false;
            }
            m_builder.reset();
            m_activeWork = work;
            m_processing = true;
            startPhase = true;
        }
    }

    if (!m_runtime->enter(error)) {
        QMutexLocker locker(&m_mutex);
        if (publishOnly) {
            m_publishing = false;
        } else if (startPhase) {
            m_processing = false;
            m_activeWork = {};
        }
        return false;
    }
    runtimeEntered = true;

    if (startPhase) {
        const bool startOk = m_processor->start(work, this, error);
        if (!startOk) {
            QMutexLocker locker(&m_mutex);
            m_processing = false;
            m_activeWork = {};
            locker.unlock();
            m_runtime->leave();
            return false;
        }
        if (runtimeEntered) {
            m_runtime->leave();
        }
        return false;
    }

    if (collectPhase) {
        if (!m_processor->isArtifactReady()) {
            if (runtimeEntered) {
                m_runtime->leave();
            }
            return false;
        }

        const bool collectOk = m_processor->collectIfReady(m_builder, error);
        if (!collectOk) {
            const bool hasError = error != nullptr && !error->isEmpty();
            QMutexLocker locker(&m_mutex);
            if (hasError) {
                m_processing = false;
                m_activeWork = {};
            }
            locker.unlock();
            if (runtimeEntered) {
                m_runtime->leave();
            }
            return false;
        }

        QMutexLocker locker(&m_mutex);
        m_processing = false;
        artifact = m_builder.snapshot();
        m_pendingWork = m_activeWork;
        m_pendingArtifact = artifact;
        m_hasPendingPublication = true;
        m_publishing = true;
        work = m_pendingWork;
        m_activeWork = {};
    }

    PublicationTicket ticket;
    const bool publishOk = m_publisher->publish(work, artifact, &ticket, error);
    if (runtimeEntered) {
        m_runtime->leave();
    }

    bool shouldPumpAgain = false;
    {
        QMutexLocker locker(&m_mutex);
        m_publishing = false;
        if (publishOk) {
            m_hasPendingPublication = false;
            m_pendingWork = {};
            m_pendingArtifact = {};
            shouldPumpAgain = m_scheduler != nullptr && m_scheduler->hasPending();
        }
    }

    if (publishOk && frameReadyCallback) {
        frameReadyCallback(ticket);
    }

    if (publishOk && shouldPumpAgain) {
        return true;
    }
    return false;
}
