#pragma once

#include "framework/platform/runtime.h"

#include <QString>

class RuntimeScope final
{
public:
    // RuntimeScope 只负责 enter/leave 配对，不负责 runtime 的 initialize/shutdown。
    RuntimeScope(IRuntime *runtime, QString *error);
    ~RuntimeScope();

    bool ok() const;

private:
    IRuntime *m_runtime = nullptr;
    bool m_entered = false;
};
