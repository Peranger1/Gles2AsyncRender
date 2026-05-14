#pragma once

#include "artifact_builder.h"
#include "work_observer.h"
#include "work_runtime.h"
#include "work_types.h"

#include <functional>
#include <QString>

using ProcessorWakeCallback = std::function<void()>;

class IWorkProcessor
{
public:
    virtual ~IWorkProcessor() = default;

    virtual bool initialize(IWorkRuntime &runtime, QString *error) = 0;
    virtual void setWakeCallback(ProcessorWakeCallback callback) = 0;
    virtual bool start(const WorkEnvelope &work,
                       IWorkObserver *observer,
                       QString *error) = 0;
    virtual bool isArtifactReady() const = 0;
    virtual bool collectIfReady(IArtifactBuilder &builder, QString *error) = 0;
    virtual void cancel(quint64 workId) = 0;
    virtual void shutdown() = 0;
};
