#pragma once

#include <QString>

class Gles2ProcTable;

class IRenderRuntime
{
public:
    virtual ~IRenderRuntime() = default;

    virtual bool initialize(QString *error) = 0;
    virtual bool makeCurrent(QString *error) = 0;
    virtual bool doneCurrent(QString *error) = 0;
    virtual void shutdown() = 0;

    virtual void *resolveProc(const char *name) const = 0;
    virtual const Gles2ProcTable &procTable() const = 0;
};
