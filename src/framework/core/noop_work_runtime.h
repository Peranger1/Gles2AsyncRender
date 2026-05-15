#pragma once

#include "work_runtime.h"

#include <QtGlobal>

class NoopWorkRuntime final : public IWorkRuntime
{
public:
    bool initialize(QString *error) override
    {
        Q_UNUSED(error);
        return true;
    }

    bool enter(QString *error) override
    {
        Q_UNUSED(error);
        return true;
    }

    void leave() override
    {
    }

    void shutdown() override
    {
    }

    void *resolveProc(const char *name) const override
    {
        Q_UNUSED(name);
        return nullptr;
    }
};
