#pragma once

#include "execution_context.h"
#include "execution_types.h"

#include <functional>
#include <memory>

enum class StartDisposition
{
    CompletedInline,
    StartedAsync,
    Failed
};

class IRequestExecution
{
public:
    virtual ~IRequestExecution() = default;

    virtual void setCompletionCallback(std::function<void()> callback) = 0;
    virtual bool collectResult(ExecutionResult *result, QString *error) = 0;
};

class IRequestHandler
{
public:
    virtual ~IRequestHandler() = default;

    virtual RequestTypeDescriptor descriptor() const = 0;
    virtual StartDisposition start(const ExecutionRequest &request,
                                   IExecutionContext &context,
                                   std::unique_ptr<IRequestExecution> *asyncExecution,
                                   ExecutionResult *inlineResult,
                                   QString *error) = 0;
    virtual void shutdown() = 0;
};
