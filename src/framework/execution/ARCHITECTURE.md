# Execution Architecture

本文描述 `src/framework/execution` 的新执行层架构。正文用中文记录，文件名保持英文，便于工程内跨平台工具和索引统一处理。

## 设计目标

- 统一异步结果表达：所有异步任务返回 `async::Future<T>`。
- 分离执行位置和业务流控：executor 只回答“在哪运行”，同类业务请求如何串行、替换或合并由业务 actor/mailbox 决定。
- 让 runtime 线程亲和性显式化：`RuntimeExecutor` 把 `IRuntime` 绑定到单独的 `SingleThreadExecutor`。
- 保持通用层无平台依赖：`future` 和 `TaskScheduler` 不依赖 Qt、OpenGL、EGL 或 `IRuntime`。
- 让失败沿 future 传播：拒绝、关闭、超时、业务异常都通过 exception 进入 `Future<T>`。

## 分层

```text
应用/业务层
  PhotoEditorAppSession
  PhotoEditorRuntimeService
  PhotoEditorHandleActor

Runtime 适配层
  execution::RuntimeExecutor
  RuntimeScope

通用提交层
  execution::TaskScheduler

异步基础层
  async::Future / Promise / Try / Unit
  async::Executor 及其实现
  collect / timeout / splitter 等 combinator
```

`execution.h` 只聚合通用 future、异常与 scheduler 能力。`runtime_executor.h` 单独存在，因为它引入 `IRuntime`、`QString` 和平台 runtime 生命周期。

## Namespace 边界

- `async`：独立 C++17 future/executor 模型，只依赖标准库。
- `execution`：项目执行策略，包含 scheduler、runtime adapter 和 execution 相关异常。
- 全局命名空间：`RuntimeScope` 当前仍在全局命名空间，作为 `IRuntime::enter()` / `leave()` 的 RAII 辅助对象。

## 数据流

普通异步任务：

```text
调用方
  -> TaskScheduler::submit()
  -> async::makeReadyFuture().via(executor)
  -> thenValue(task)
  -> Future<Result>
```

runtime 任务：

```text
调用方
  -> RuntimeExecutor::submit(lambda(IRuntime&))
  -> SingleThreadExecutor("runtime")
  -> lambda 在 runtime worker thread 上运行
  -> Future<Result>
```

## 线程模型

- `InlineExecutor` 立即在当前调用栈执行任务。
- `ThreadExecutor` 为每个任务创建 detached 标准线程。
- `ThreadPoolExecutor` 使用固定 worker 池。
- `ManualExecutor` 只入队，由测试显式 `drain()`。
- `SerialExecutor` 包装另一个 executor，保证 FIFO 且不内联重入。
- `SingleThreadExecutor` 使用一个专属 worker thread，保证 FIFO，适合 `IRuntime`、EGL/GLES2 等线程亲和对象。

`RuntimeExecutor` 默认创建名为 `"runtime"` 的 `SingleThreadExecutor`。`initialize()`、`submit()`、`shutdown()` 都通过这个 executor 进入同一条线程。

## 错误模型

execution 层不返回状态结构。失败被写入 `Future<T>`，调用方通过 `get()` 抛出或 `thenTry()` 观察。

常用异常：

- `async::FutureInvalid`：future 已被消费、无状态或输入 future 非法。
- `async::BrokenPromise`：producer 销毁时还没有完成 promise。
- `async::FutureTimeout`：`within()` 超时胜出。
- `execution::ExecutorShutdown`：scheduler 或 runtime executor 已关闭。
- `execution::TaskRejected`：任务无法被当前 executor/scheduler 接受。
- `execution::RuntimeUnavailable`：runtime 或 runtime executor 不可用。
- `execution::RuntimeInitializeFailed`：`IRuntime::initialize()` 返回失败。

## 业务流控

execution 层不再提供 lane。photo editor 的 latest-only、process-again、destroy 优先级和 generation 防护由 `PhotoEditorHandleActor` 实现；平台纹理发布容量由 writer/reader 交换链路处理。这样 runtime executor 保持与 folly executor 类似的边界：只负责执行位置和顺序，不理解业务请求类型。

## Runtime 生命周期

```text
create RuntimeExecutor
  -> initialize()
  -> submit(...) 多次
  -> shutdown()
  -> destructor 兜底 shutdown
```

`RuntimeExecutor` 独占 `std::unique_ptr<IRuntime>`。shutdown 后 runtime 会被 `shutdown()` 并释放。shutdown 是幂等的，重复调用返回 ready future。

析构函数会尽力执行 shutdown，但会吞掉异常。业务层需要确定 shutdown 结果时，应显式调用 `shutdown().get()`。

## 和应用层的关系

Photo editor 主路径现在通过 `PhotoEditorAppSession` 创建 `RuntimeExecutor`，再把同一个 executor 传给 runtime service、actor 和 writer。这样 GL 创建、处理、渲染、纹理发布等 runtime 相关操作都经过同一条 runtime executor 路径，而不是混用 Qt queued call 和旧 host 接口。
