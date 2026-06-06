# Execution

The execution layer is built around three independent concepts:

- `async::Future` / `async::Promise`: asynchronous result delivery.
- `async::Executor`: where work runs.
- lanes: how streams of submitted work are ordered, replaced, or merged.

## Executors

Use the executor types from `future/async_future.h`.

- `InlineExecutor`
- `ThreadExecutor`
- `ThreadPoolExecutor`
- `ManualExecutor`
- `SerialExecutor`
- `SingleThreadExecutor`

`ManualExecutor` is intended for deterministic tests.

`SerialExecutor` wraps another executor and guarantees FIFO one-at-a-time
execution without inline reentrancy.

`SingleThreadExecutor` is the expected executor for thread-affine resources such
as EGL/GLES2 runtime objects.

## Scheduler

`execution::TaskScheduler` binds a generic `async::Executor` and exposes
`submit()` returning `async::Future<T>`.

```cpp
execution::TaskScheduler scheduler(executor);
auto future = scheduler.submit([] {
    return 42;
});
```

## Lanes

- `SerialLane<Args, Result>` runs every task one at a time, in FIFO order.
- `LatestLane<Args, Result>` keeps only the latest waiting task.
- `LatestLane<Args, Result, LatestLaneDelivery::MarkActiveStaleWhenWaiting>`
  marks an active result as `TaskStale` when a newer task is already waiting.
- `MergeLane<Args, Result, Merger>` merges one waiting task with newer input.

All lane submissions return futures. Rejected, superseded, stale, or shutdown
tasks complete through exceptions rather than callback-specific status structs.

## Runtime Executor

`execution::RuntimeExecutor` owns an `IRuntime` and schedules all runtime calls
on a `SingleThreadExecutor`.

```cpp
#include "framework/execution/runtime_execution.h"

execution::RuntimeExecutor runtimeExecutor(std::move(runtime));

runtimeExecutor.initialize().get();

auto result = runtimeExecutor.submit([](IRuntime& runtime) {
    return doRuntimeWork(runtime);
});
```

The core `execution.h` aggregate does not include runtime adapters, so generic
execution code remains independent from `IRuntime` and Qt string types.

The legacy `SubmitResult`, `ExecutionOutcome`, and callback lane model has been
removed from the clean execution API.
