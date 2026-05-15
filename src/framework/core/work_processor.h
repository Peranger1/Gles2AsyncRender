#pragma once

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
    virtual bool isOutputReady() const = 0;
    virtual bool collectOutputIfReady(ProcessorOutput *output, QString *error) = 0;
    virtual void shutdown() = 0;
};
