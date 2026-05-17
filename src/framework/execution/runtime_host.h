#pragma once

#include "framework/platform/runtime.h"

#include <QString>

#include <functional>

class RuntimeHost
{
public:
    using RuntimeClosure = std::function<void(IRuntime *)>;

    virtual ~RuntimeHost() = default;

    virtual bool start(QString *error) = 0;
    virtual void shutdown() = 0;

    virtual IRuntime *runtime() const = 0;
    virtual bool isOnRuntimeThread() const = 0;
    virtual bool dispatchAsync(RuntimeClosure closure, QString *error) = 0;
    virtual bool dispatchSync(RuntimeClosure closure, QString *error) = 0;
};
