#pragma once

#include "framework/execution/task_scheduler.h"

namespace execution
{
template <typename Result>
Result syncGet(async::Future<Result> future)
{
    return future.get();
}
}
