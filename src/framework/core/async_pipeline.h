#pragma once

#include "frame_publisher.h"
#include "work_processor.h"
#include "work_scheduler.h"

#include <functional>

class IAsyncPipeline
{
public:
    virtual ~IAsyncPipeline() = default;

    virtual bool initialize(IWorkRuntime *runtime,
                            IWorkProcessor *processor,
                            IFramePublisher *publisher,
                            IFrameWriter *frameWriter,
                            IWorkScheduler *scheduler,
                            QString *error) = 0;

    virtual void submit(const WorkEnvelope &work) = 0;
    virtual void pump() = 0;
    virtual void onPublicationCapacityAvailable() = 0;
    virtual void setStateChangedCallback(std::function<void(RequestId, WorkState)> callback) = 0;
    virtual void setProgressCallback(std::function<void(quint64, int, bool)> callback) = 0;
    virtual void setMessageCallback(std::function<void(RequestId, const QString &)> callback) = 0;
    virtual void setResultReadyCallback(std::function<void(const JobResult &)> callback) = 0;
    virtual void setErrorCallback(std::function<void(const QString &)> callback) = 0;
    virtual void shutdown() = 0;
};
