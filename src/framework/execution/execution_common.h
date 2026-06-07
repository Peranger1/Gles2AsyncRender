#pragma once

#include "framework/execution/future/exceptions.h"

namespace execution
{
class ExecutorShutdown : public async::FutureException
{
public:
    ExecutorShutdown()
        : async::FutureException("Executor shutdown")
    {
    }
};

class TaskRejected : public async::FutureException
{
public:
    TaskRejected()
        : async::FutureException("Task rejected")
    {
    }
};
}
