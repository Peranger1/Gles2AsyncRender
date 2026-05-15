#include "serial_conflated_async_pipeline.h"

#include <QMutexLocker>

#include <variant>

bool SerialConflatedAsyncPipeline::initialize(IWorkRuntime *runtime,
                                              IWorkProcessor *processor,
                                              IFramePublisher *publisher,
                                              IFrameWriter *frameWriter,
                                              IWorkScheduler *scheduler,
                                              QString *error)
{
    if (runtime == nullptr || processor == nullptr || scheduler == nullptr) {
        if (error) {
            *error = QStringLiteral("SerialConflatedAsyncPipeline requires runtime, processor, and scheduler.");
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
    bool publisherReady = true;
    if (processorReady) {
        if (m_publisher != nullptr) {
            if (frameWriter == nullptr) {
                if (error) {
                    *error = QStringLiteral("SerialConflatedAsyncPipeline requires a frame writer when a publisher is configured.");
                }
                shutdown();
                return false;
            }
            if (!m_runtime->enter(error)) {
                shutdown();
                return false;
            }

            publisherReady = m_publisher->initialize(*m_runtime, *frameWriter, error);
            m_runtime->leave();
        }
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

void SerialConflatedAsyncPipeline::submit(const WorkEnvelope &work)
{
    if (!m_initialized || m_scheduler == nullptr) {
        return;
    }

    m_scheduler->submit(work);
    pump();
}

void SerialConflatedAsyncPipeline::pump()
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

void SerialConflatedAsyncPipeline::onPublicationCapacityAvailable()
{
    if (!m_initialized) {
        return;
    }

    pump();
}

void SerialConflatedAsyncPipeline::setStateChangedCallback(std::function<void(RequestId, WorkState)> callback)
{
    QMutexLocker locker(&m_mutex);
    m_stateChangedCallback = std::move(callback);
}

void SerialConflatedAsyncPipeline::setProgressCallback(std::function<void(quint64, int, bool)> callback)
{
    QMutexLocker locker(&m_mutex);
    m_progressCallback = std::move(callback);
}

void SerialConflatedAsyncPipeline::setMessageCallback(std::function<void(RequestId, const QString &)> callback)
{
    QMutexLocker locker(&m_mutex);
    m_messageCallback = std::move(callback);
}

void SerialConflatedAsyncPipeline::setResultReadyCallback(std::function<void(const JobResult &)> callback)
{
    QMutexLocker locker(&m_mutex);
    m_resultReadyCallback = std::move(callback);
}

void SerialConflatedAsyncPipeline::setErrorCallback(std::function<void(const QString &)> callback)
{
    QMutexLocker locker(&m_mutex);
    m_errorCallback = std::move(callback);
}

void SerialConflatedAsyncPipeline::shutdown()
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
    m_pumpActive = false;
    m_pumpRequested = false;
    m_stateChangedCallback = {};
    m_progressCallback = {};
    m_messageCallback = {};
    m_resultReadyCallback = {};
    m_errorCallback = {};
}

void SerialConflatedAsyncPipeline::onStateChanged(RequestId requestId, WorkState state)
{
    std::function<void(RequestId, WorkState)> callback;
    {
        QMutexLocker locker(&m_mutex);
        callback = m_stateChangedCallback;
    }
    if (callback) {
        callback(requestId, state);
    }
}

void SerialConflatedAsyncPipeline::onProgress(RequestId requestId, int progress, bool isFinal)
{
    std::function<void(quint64, int, bool)> callback;
    {
        QMutexLocker locker(&m_mutex);
        callback = m_progressCallback;
    }
    if (callback) {
        callback(requestId, progress, isFinal);
    }
}

void SerialConflatedAsyncPipeline::onMessage(RequestId requestId, const QString &message)
{
    std::function<void(RequestId, const QString &)> callback;
    {
        QMutexLocker locker(&m_mutex);
        callback = m_messageCallback;
    }
    if (callback) {
        callback(requestId, message);
    }
}

bool SerialConflatedAsyncPipeline::shouldDeliverResult(const WorkEnvelope &completedWork) const
{
    if (m_scheduler == nullptr) {
        return true;
    }

    switch (completedWork.hints.deliveryPolicy) {
    case ResultDeliveryPolicy::AlwaysDeliver:
        return true;
    case ResultDeliveryPolicy::DeliverOnlyIfNoPending:
    case ResultDeliveryPolicy::DeliverOnlyIfLatest:
        return !m_scheduler->hasPendingForLane(completedWork.laneId);
    default:
        return true;
    }
}

bool SerialConflatedAsyncPipeline::pumpStep(QString *error)
{
    std::function<void(const JobResult &)> resultReadyCallback;
    WorkEnvelope work;
    ProcessorOutput output;
    bool publishOnly = false;
    bool collectPhase = false;
    bool startPhase = false;
    bool runtimeEntered = false;

    {
        QMutexLocker locker(&m_mutex);
        if (!m_initialized || m_runtime == nullptr || m_processor == nullptr
            || m_scheduler == nullptr || m_scheduler->isPublishing()) {
            return false;
        }

        resultReadyCallback = m_resultReadyCallback;

        if (m_scheduler->takePublication(&work, &output)) {
            publishOnly = true;
        } else if (m_scheduler->activeWork(&work)) {
            collectPhase = true;
        } else {
            if (!m_scheduler->takeNext(&work)) {
                return false;
            }
            startPhase = true;
        }
    }

    if (!m_runtime->enter(error)) {
        if (publishOnly) {
            m_scheduler->markPublicationDeferred(work);
        } else if (startPhase) {
            m_scheduler->markActiveFinished(work, nullptr);
        }
        return false;
    }
    runtimeEntered = true;

    if (startPhase) {
        const bool startOk = m_processor->start(work, this, error);
        if (!startOk) {
            m_scheduler->markActiveFinished(work, nullptr);
            m_runtime->leave();
            return false;
        }
        if (runtimeEntered) {
            m_runtime->leave();
        }
        return true;
    }

    if (collectPhase) {
        if (!m_processor->isOutputReady()) {
            if (runtimeEntered) {
                m_runtime->leave();
            }
            return false;
        }

        ProcessorOutput collectedOutput;
        const bool collectOk = m_processor->collectOutputIfReady(&collectedOutput, error);
        if (!collectOk) {
            const bool hasError = error != nullptr && !error->isEmpty();
            if (hasError) {
                m_scheduler->markActiveFinished(work, nullptr);
            }
            if (runtimeEntered) {
                m_runtime->leave();
            }
            return false;
        }

        m_scheduler->markActiveFinished(work, &collectedOutput);
        if (runtimeEntered) {
            m_runtime->leave();
        }
        return true;
    }

    const bool shouldDeliver = shouldDeliverResult(work);
    if (!shouldDeliver) {
        if (runtimeEntered) {
            m_runtime->leave();
        }
        m_scheduler->markPublicationFinished(work);
        return m_scheduler != nullptr
            && (m_scheduler->hasPublicationPending() || m_scheduler->hasPending());
    }

    JobResult jobResult;
    jobResult.requestId = work.requestId;
    bool publishOk = false;
    if (std::holds_alternative<GpuTextureResult>(output.payload)) {
        if (m_publisher == nullptr) {
            if (error) {
                *error = QStringLiteral("SerialConflatedAsyncPipeline cannot publish a GPU texture result without a frame publisher.");
            }
            if (runtimeEntered) {
                m_runtime->leave();
            }
            m_scheduler->markPublicationFinished(work);
            return false;
        }
        FrameTicket ticket;
        const bool published = m_publisher->publish(work,
                                                    std::get<GpuTextureResult>(output.payload),
                                                    &ticket,
                                                    error);
        if (published) {
            jobResult.resultKind = output.outputKind.isEmpty() ? QStringLiteral("frame") : output.outputKind;
            jobResult.payload = ticket;
            publishOk = true;
        }
    } else if (std::holds_alternative<CpuImageResult>(output.payload)) {
        jobResult.resultKind = output.outputKind;
        jobResult.payload = std::get<CpuImageResult>(output.payload);
        publishOk = true;
    } else if (std::holds_alternative<std::shared_ptr<ICustomResult>>(output.payload)) {
        jobResult.resultKind = output.outputKind;
        jobResult.payload = std::get<std::shared_ptr<ICustomResult>>(output.payload);
        publishOk = true;
    }
    if (runtimeEntered) {
        m_runtime->leave();
    }

    bool shouldPumpAgain = false;
    if (publishOk) {
        m_scheduler->markPublicationFinished(work);
        shouldPumpAgain = m_scheduler != nullptr
            && (m_scheduler->hasPublicationPending() || m_scheduler->hasPending());
    } else {
        const bool shouldRetryPublication = error == nullptr || error->isEmpty();
        if (shouldRetryPublication) {
            m_scheduler->markPublicationDeferred(work);
        } else {
            m_scheduler->markPublicationFinished(work);
        }
    }

    if (publishOk && resultReadyCallback) {
        resultReadyCallback(jobResult);
    }

    return publishOk && shouldPumpAgain;
}
