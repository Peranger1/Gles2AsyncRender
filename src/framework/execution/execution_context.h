#pragma once

class IRuntime;

class IExecutionContext
{
public:
    virtual ~IExecutionContext() = default;

    virtual IRuntime *runtime() const = 0;
};
