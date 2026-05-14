#pragma once

#include <QString>

class IWorkRuntime
{
public:
    virtual ~IWorkRuntime() = default;

    virtual bool initialize(QString *error) = 0;
    virtual bool enter(QString *error) = 0;
    virtual void leave() = 0;
    virtual void shutdown() = 0;

    virtual void *resolveProc(const char *name) const = 0;
};
