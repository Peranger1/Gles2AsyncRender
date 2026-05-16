#pragma once

#include "execution_context.h"
#include "execution_types.h"

#include <QString>

class IRequestResultSink
{
public:
    virtual ~IRequestResultSink() = default;

    virtual void onResultReady(const ExecutionResult &result, IExecutionContext &context) = 0;
    virtual void onRequestFailed(RequestId requestId, const QString &error) = 0;
};
