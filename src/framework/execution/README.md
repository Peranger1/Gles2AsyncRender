# Execution

本目录是项目的新异步执行层。它把“任务怎么完成”“任务在哪个线程执行”“连续提交的任务如何排队或合并”拆成独立概念，避免业务层继续依赖旧的 `RuntimeHost` 回调式模型。

## 文档

- [ARCHITECTURE.md](ARCHITECTURE.md)：说明 execution 层的分层、边界、线程模型和错误模型。
- [IMPLEMENTATION.md](IMPLEMENTATION.md)：说明 future、executor、lane、runtime adapter 的实现细节和约束。

## 入口头

- `framework/execution/future/async_future.h`：只使用 future/executor 能力时包含它。
- `framework/execution/execution.h`：使用通用 scheduler 和 lane 时包含它。
- `framework/execution/runtime_executor.h`：需要访问 `IRuntime` 时直接包含它。

`execution.h` 不包含 `runtime_executor.h`，这样通用 execution 代码不会被 Qt 字符串和平台 runtime 接口污染。

## 核心概念

- `async::Promise<T>` / `async::Future<T>`：一次性异步结果通道。
- `async::Executor`：continuation 或任务实际运行的位置。
- `execution::TaskScheduler`：把普通函数提交到指定 executor，并返回 `Future<T>`。
- `execution::SerialLane` / `LatestLane` / `MergeLane`：描述同一类任务连续提交时的排队、替换、合并策略。
- `execution::RuntimeExecutor`：拥有一个 `IRuntime`，并把所有 runtime 调用固定到一个 `SingleThreadExecutor`。

## 最小示例

```cpp
#include "framework/execution/runtime_executor.h"

auto executor = std::make_unique<execution::RuntimeExecutor>(backend->createRuntime());
executor->initialize().get();

auto future = executor->submit([](IRuntime &runtime) {
    return doRuntimeWork(runtime);
});

auto result = future.get();
executor->shutdown().get();
```

## 旧模型状态

旧的 `RuntimeHost`、`QtRuntimeHost`、`SubmitResult`、`ExecutionOutcome` 和 callback lane 模型已经从当前 execution 主路径移除。新代码应返回 `async::Future<T>`，失败通过 exception 存入 future，而不是额外的状态结构。
