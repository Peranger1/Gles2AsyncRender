# Execution Implementation

本文记录 execution 层的关键实现细节、约束和维护注意事项。

## Future 状态机

`async::Future<T>` 和 `async::Promise<T>` 共享一个 `SharedState<T>`。

`SharedState<T>` 保存：

- `std::optional<Try<T>> result`
- 一个 continuation callback
- 一个 continuation executor
- 一个 interrupt handler
- `condition_variable`，供 `Future::get()` 阻塞等待
- 状态位：ready、futureRetrieved

`Promise<T>::getFuture()` 只能调用一次。`Future<T>` 是 move-only 且单消费者，`get()`、`getFor()`、`getUntil()`、`thenValue()`、`thenTry()`、`thenError()` 都会消费源 future。消费后再使用会抛 `FutureInvalid`。`waitFor()` / `waitUntil()` 只等待 ready，不消费结果。

## Try 和 Unit

`async::Try<T>` 表示 value 或 `std::exception_ptr`。`Future<T>::get()` 最终取出 `Try<T>` 并调用 `value()`，如果保存的是异常就重新抛出。

`async::Unit` 是逻辑 void。producer 或 continuation 返回 `void` 时会被提升为 `Future<Unit>`，这样接口不需要为 void 特化出两套语义。

## Continuation 执行

`Future::via(executor)` 设置当前 future 的 continuation executor。随后在这个 future 上注册的 `thenValue()` / `thenTry()` / `thenError()` / `ensure()` callback 会通过该 executor 调度；如果没有 executor，则回退到 `InlineExecutor`。

这是轻量调度语义，不是 Folly 那种完整 executor policy。`via()` 不会把 executor 自动复制到所有下游 future。普通同步链路里，下一个 continuation 通常会在上一个 continuation 完成结果的线程内 inline 执行；如果 continuation 返回另一个异步 `Future<T>`，后续 continuation 会跟随该 inner future 的完成线程。需要固定新的异步边界时，在对应 future 上再次调用 `.via(executor)`。

`async::async(executor, func)` 的 producer 和 continuation 都支持三类返回值：

- 普通值：直接兑现输出 `Future<Result>`。
- `void`：兑现输出为 `Future<Unit>`。
- `Future<Result>`：自动 flatten，输出 future 等待内层 future 完成。

callback 和 interrupt handler 都在锁外执行，避免用户代码在 shared state 锁内重入。

如果 executor 拒绝调度，continuation 的下游 future 会以 `ExecutorRejected` 完成，避免调用方永久等待。`async(executor, func)` 的初始投递也遵循同一规则。

`thenError(F(std::exception_ptr))` 仍保留原有通用错误恢复形式。`thenError<E>(F)` 会只处理匹配 `E` 的异常；不匹配时原异常继续向下游传播。typed handler 和其他 continuation 一样支持返回普通值、`void` 或 `Future<T>`。

`ensure(F)` 是同步清理钩子：上游成功或失败都会执行 `F`，然后继续传递原结果。`F` 如果抛异常，下游 future 以该异常完成。当前轻量实现不会等待 `F` 返回的 future；需要异步清理时应显式写成普通 continuation 链。

## Promise 生命周期

`Promise<T>` 析构时如果 shared state 仍未 ready，会以 `BrokenPromise` 完成 future。这让 producer 提前销毁不会导致 consumer 永久等待。一个例外是 `Promise::setWith()` 的 producer 返回 pending `Future<T>` 时，外层 promise 已经把完成责任转交给 inner future；这时 promise 析构不会写入 `BrokenPromise`。

重复完成 promise 会抛 `PromiseAlreadySatisfied`。重复获取 future 会抛 `FutureAlreadyRetrieved`。

`Future::cancel()` / `Future::raise()` 只把 interrupt 交给 producer 注册的 handler，不会自动完成 future。interrupt 是 advisory signal：是否停止生产、是否忽略、如何完成结果，都由 producer 决定。若没有注册 interrupt handler，interrupt 会被保存，并在 handler 后续注册时投递；future 已完成后再 interrupt 不会改变结果。

## Executor 实现

`async::Executor` 的核心投递方法返回 `bool`，用于反馈任务是否被接受：

```cpp
virtual bool add(std::function<void()> task) = 0;
```

executor 基类还提供最小观测接口：

```cpp
virtual bool isShutdown() const;
virtual int queuedCount() const;
virtual const char *typeName() const;
```

当前实现：

- `InlineExecutor`：立即执行，主要作为默认 fallback。
- `ThreadExecutor`：每个任务一个 detached thread，适合简单异步测试，不适合高频任务。
- `ThreadPoolExecutor`：固定 worker 池，析构时停止并 join。
- `ManualExecutor`：只入队，测试用 `drainOne()` / `drain()` 控制执行时机。
- `SerialExecutor`：包装另一个 executor，内部队列保证 FIFO 和非重入。
- `SingleThreadExecutor`：一个专属 worker，支持 `isOnExecutorThread()` 和 `shutdown()`。

空 task、shutdown 后投递、底层 executor 拒绝、线程创建失败等路径会返回 `false`。各 executor 的 worker 边界会捕获用户 task 抛出的异常，避免 executor 线程意外退出。

`SingleThreadExecutor::shutdown()` 如果从自身 worker thread 调用，会 detach 当前 worker 后返回，避免 join 自己造成死锁。外部线程调用会 join worker。

## Combinator

- `makeReadyFuture()` / `makeReadyFuture(value)`：创建已完成 future。
- `makeExceptionFuture<T>()`：创建已失败 future。
- `collectAll()`：等待全部完成，返回 `vector<Try<T>>`，不因单个失败提前失败。
- `collect()`：基于 `collectAll()`，返回 `vector<T>`，任一结果为异常时在取值时抛出。
- `collectAny()`：第一个完成者胜出，返回 index 和 `Try<T>`。
- `sleepFor()` / `sleepUntil()`：注册到共享 `TimerExecutor`，由单 worker thread 按 deadline 调度。
- `within()`：原 future 和 timeout 竞争，先完成者兑现输出 future；timeout 胜出时会向原 future 发送 `FutureTimeout` interrupt。
- `delayed()`：原 future 完成后再等待指定时间，然后传递原值或原异常。
- `onTimeout()`：基于 `within()`，只恢复 `FutureTimeout`，支持返回 fallback 值或 fallback `Future<T>`。
- `FutureSplitter<T>`：把单消费者 future 转成多个 future，当前要求 `T` 可拷贝。

`TimerExecutor` 使用 deadline 最小堆和 `TimerTaskHandle` 取消句柄。原 future 先完成时，`within()` 会取消对应 timer；timeout 先完成时，只通知 producer，不强制终止底层任务。`onTimeout()` 虽然可以给调用方返回 fallback 结果，也同样不会强杀原 producer。producer 仍需要通过 interrupt handler 自行决定是否停止和如何完成原 promise。

## TaskScheduler

`execution::TaskScheduler` 持有一个 `std::shared_ptr<async::Executor>`。

`submit(F)` 的实现路径是：

```cpp
makeReadyFuture()
  .via(executor)
  .thenValue(F)
```

如果 scheduler 已 shutdown，返回保存 `ExecutorShutdown` 的 failed future。如果 executor 为空，返回保存 `TaskRejected` 的 failed future。

## RuntimeExecutor

`RuntimeExecutor` 内部 `State` 包含：

- `std::unique_ptr<IRuntime> runtime`
- `std::shared_ptr<async::SingleThreadExecutor> executor`
- `std::atomic<bool> initialized`
- `std::atomic<bool> shutdown`

构造时如果没有传入 executor，会创建默认 `SingleThreadExecutor("runtime")`。

`initialize()` 投递到 runtime executor 上执行。它只初始化一次，`initialized` 已为 true 时直接返回。`IRuntime::initialize()` 失败会转换为 `RuntimeInitializeFailed`。

`submit(F)` 接收 `F(IRuntime&)`，返回 `Future<Result>`。提交前会检查 shutdown、runtime、executor 状态；任务真正运行时还会再次检查 shutdown 和 runtime，避免排队后被关闭。

`submitBlocking(F)` 用于必须同步完成的路径。它有两个分支：

- 如果当前已经在 runtime executor thread 上，直接调用 F，避免自己等待自己。
- 否则调用 `submit(F).get()` 阻塞等待。

`F` 的返回值语义和 `submit(F)` 保持一致：普通值直接返回，`void` 转为 `Unit`，`Future<T>` 会被同步 flatten 成 `T`。因此 inline 分支也会对返回的 future 调用 `get()`；调用方不能在 runtime thread 上返回一个还依赖同一 runtime executor 才能完成的 pending future，否则仍可能造成业务层面的自等待。

`shutdown()` 使用 `shutdown.exchange(true)` 保证只执行一次。executor 可用时，runtime 的 shutdown 和释放也在 runtime thread 上执行；executor 不可用时直接在当前线程兜底释放 runtime。

## RuntimeScope

`RuntimeScope` 是 `IRuntime::enter()` / `leave()` 的 RAII 包装。

构造时调用 `enter(error)`，析构时如果 enter 成功则调用 `leave()`。它不拥有 runtime，也不负责 initialize/shutdown。调用方应在 `RuntimeExecutor::submit()` 的 lambda 内创建它。

## 源码注释约定

源码注释使用中文，重点说明 API 约束、线程边界、生命周期转移和非显然状态转换。测试文件应在文件顶部说明覆盖范围，并在复杂边界用例前说明验证目的。不要给简单赋值、直接返回或测试名已经清楚表达的断言逐行加注释。

## 维护约束

- 不要在通用 execution 头里引入 Qt 或平台 GL 头。
- 新的异步 API 应返回 `async::Future<T>`，不要恢复 callback result struct。
- 业务请求的 latest-only、合并、丢弃和 stale 过滤应放在 actor/mailbox 或平台交换链路，不放在 execution 层。
- 在 runtime thread 上不要调用会等待同一 runtime executor 的同步方法，除非方法像 `submitBlocking()` 一样显式处理了重入。
- `Future<T>` 是单消费者；需要多消费者时使用 `FutureSplitter<T>`，并确认 T 可拷贝。
- shutdown 路径应优先显式调用并观察结果，析构里的 shutdown 只是兜底。
