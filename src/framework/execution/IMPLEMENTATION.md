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

`Promise<T>::getFuture()` 只能调用一次。`Future<T>` 是 move-only 且单消费者，`get()`、`thenValue()`、`thenTry()`、`thenError()` 都会消费源 future。消费后再使用会抛 `FutureInvalid`。

## Try 和 Unit

`async::Try<T>` 表示 value 或 `std::exception_ptr`。`Future<T>::get()` 最终取出 `Try<T>` 并调用 `value()`，如果保存的是异常就重新抛出。

`async::Unit` 是逻辑 void。continuation 返回 `void` 时会被提升为 `Future<Unit>`，这样接口不需要为 void 特化出两套语义。

## Continuation 执行

`Future::via(executor)` 设置当前 future 的 continuation executor。后续 `thenValue()` / `thenTry()` 注册的 callback 会通过该 executor 调度；如果没有 executor，则回退到 `InlineExecutor`。

continuation 支持三类返回值：

- 普通值：直接兑现下一个 `Future<Result>`。
- `void`：兑现为 `Future<Unit>`。
- `Future<Result>`：自动 flatten，外层 future 等待内层 future 完成。

callback 和 interrupt handler 都在锁外执行，避免用户代码在 shared state 锁内重入。

## Promise 生命周期

`Promise<T>` 析构时如果 shared state 仍未 ready，会以 `BrokenPromise` 完成 future。这让 producer 提前销毁不会导致 consumer 永久等待。

重复完成 promise 会抛 `PromiseAlreadySatisfied`。重复获取 future 会抛 `FutureAlreadyRetrieved`。

`Future::cancel()` / `Future::raise()` 只把 interrupt 交给 producer 注册的 handler，不会自动完成 future。是否停止生产、如何完成结果，由 producer 决定。

## Executor 实现

`async::Executor` 只有一个方法：

```cpp
virtual void add(std::function<void()> task) = 0;
```

当前实现：

- `InlineExecutor`：立即执行，主要作为默认 fallback。
- `ThreadExecutor`：每个任务一个 detached thread，适合简单异步测试，不适合高频任务。
- `ThreadPoolExecutor`：固定 worker 池，析构时停止并 join。
- `ManualExecutor`：只入队，测试用 `drainOne()` / `drain()` 控制执行时机。
- `SerialExecutor`：包装另一个 executor，内部队列保证 FIFO 和非重入。
- `SingleThreadExecutor`：一个专属 worker，支持 `isOnExecutorThread()` 和 `shutdown()`。

`SingleThreadExecutor::shutdown()` 如果从自身 worker thread 调用，会 detach 当前 worker 后返回，避免 join 自己造成死锁。外部线程调用会 join worker。

## Combinator

- `makeReadyFuture()` / `makeReadyFuture(value)`：创建已完成 future。
- `makeExceptionFuture<T>()`：创建已失败 future。
- `collectAll()`：等待全部完成，返回 `vector<Try<T>>`，不因单个失败提前失败。
- `collect()`：基于 `collectAll()`，返回 `vector<T>`，任一结果为异常时在取值时抛出。
- `collectAny()`：第一个完成者胜出，返回 index 和 `Try<T>`。
- `sleepFor()` / `sleepUntil()`：使用 detached 标准线程完成定时。
- `within()`：原 future 和 timeout 竞争，先完成者兑现输出 future。
- `FutureSplitter<T>`：把单消费者 future 转成多个 future，当前要求 `T` 可拷贝。

`sleepFor()`、`sleepUntil()`、`within()` 当前为第一版实现，定时依赖 detached 标准线程，不适合大量高频 timer。

## TaskScheduler

`execution::TaskScheduler` 持有一个 `std::shared_ptr<async::Executor>`。

`submit(F)` 的实现路径是：

```cpp
makeReadyFuture()
  .via(executor)
  .thenValue(F)
```

如果 scheduler 已 shutdown，返回保存 `ExecutorShutdown` 的 failed future。如果 executor 为空，返回保存 `TaskRejected` 的 failed future。

## Lane 实现

三种 lane 都以 `std::shared_ptr<State>` 保存共享状态，保证 lane 对象移动或外部 future continuation 晚完成时状态仍然有效。

共有状态：

- `std::mutex mutex`
- `Runner runner`
- `bool active`
- `bool shutdown`
- waiting 容器

`submit()` 只在锁内决定状态转换，不在锁内调用 runner。runner 返回的 future 完成后，`thenTry()` 兑现 submit 对应的 promise，然后调用 `startNext()`。

### SerialLane

waiting 是 `std::deque<Pending>`。active 任务运行时，新任务全部排队。active 完成后取队首继续执行。

### LatestLane

waiting 是 `std::optional<Pending>`。active 任务运行时，新任务替换旧 waiting。被替换的 waiting promise 得到 `TaskSuperseded`。

`LatestLaneDelivery::MarkActiveStaleWhenWaiting` 会在 active 结果完成时检查是否存在 waiting。如果存在，active promise 得到 `TaskStale`，避免过期结果继续向业务层传播。

### MergeLane

waiting 是 `std::optional<Pending>`。当已有 waiting 时，新任务先调用：

```cpp
merger.canMerge(waiting.args, incoming.args)
```

如果允许合并，旧 waiting promise 得到 `TaskSuperseded`，新 waiting args 变成：

```cpp
merger.merge(oldArgs, incomingArgs)
```

如果不能合并，新任务 promise 得到 `TaskRejected`。

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

`shutdown()` 使用 `shutdown.exchange(true)` 保证只执行一次。executor 可用时，runtime 的 shutdown 和释放也在 runtime thread 上执行；executor 不可用时直接在当前线程兜底释放 runtime。

## RuntimeScope

`RuntimeScope` 是 `IRuntime::enter()` / `leave()` 的 RAII 包装。

构造时调用 `enter(error)`，析构时如果 enter 成功则调用 `leave()`。它不拥有 runtime，也不负责 initialize/shutdown。调用方应在 `RuntimeExecutor::submit()` 的 lambda 内创建它。

## 维护约束

- 不要在通用 execution 头里引入 Qt 或平台 GL 头。
- 新的异步 API 应返回 `async::Future<T>`，不要恢复 callback result struct。
- lane 的 runner 不应返回 invalid future；需要拒绝时返回 failed future。
- 在 runtime thread 上不要调用会等待同一 runtime executor 的同步方法，除非方法像 `submitBlocking()` 一样显式处理了重入。
- `Future<T>` 是单消费者；需要多消费者时使用 `FutureSplitter<T>`，并确认 T 可拷贝。
- shutdown 路径应优先显式调用并观察结果，析构里的 shutdown 只是兜底。
