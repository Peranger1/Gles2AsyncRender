#pragma once

#include "framework/platform/runtime.h"

#include <QString>

class RuntimeScope final
{
public:
    RuntimeScope(IRuntime *runtime, QString *error);
    ~RuntimeScope();

    bool ok() const;

private:
    IRuntime *m_runtime = nullptr;
    bool m_entered = false;
};
