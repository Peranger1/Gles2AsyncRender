# Execution Usage Examples

本文给出 `src/framework/execution` 的常见用法。execution 层的边界是：

- `async` 负责 future、promise、executor、combinator。
- `execution` 负责项目级提交 helper、通用异常和 runtime executor。
- 业务请求的 latest-only、merge、drop、stale 过滤放在 actor/mailbox 或平台交换链路，不放在 execution 层。

示例中的 `parseText()`、`ParsedData`、`asyncReadFile()`、`slowRead()`、`parseBytes()`、`doOneStep()` 等名称是业务占位符，不属于 execution API。

## 入口头

只使用 future/executor：

```cpp
#include "framework/execution/future/async_future.h"
```

使用 `TaskScheduler` 和 execution 通用异常：

```cpp
#include "framework/execution/execution.h"
```

提交需要 `IRuntime` 的 GL task：

```cpp
#include "framework/execution/runtime_executor.h"
#include "framework/execution/runtime_scope.h"
```

## Future 和 Promise

`Future<T>` 是单消费者异步结果。`Promise<T>` 是 producer 端，只能交出一个 future。

```cpp
async::Promise<int> promise;
async::Future<int> future = promise.getFuture();

promise.setValue(42);

const int value = future.get(); // 42
```

`get()` 会消费 future。消费后继续调用 `get()`、`thenValue()`、`via()` 等操作会抛 `async::FutureInvalid`。

```cpp
auto future = async::makeReadyFuture(42);
const int value = future.get();

// future.get(); // 会抛 async::FutureInvalid
```

`Future<Unit>` 表示异步 void。

```cpp
async::Promise<async::Unit> promise;
auto future = promise.getFuture();

promise.setValue();
future.get();
```

producer 提前销毁且没有完成 promise 时，consumer 会得到 `async::BrokenPromise`。

```cpp
async::Future<int> future;

{
    async::Promise<int> promise;
    future = promise.getFuture();
} // promise 析构，future 变成 BrokenPromise

try {
    future.get();
} catch (const async::BrokenPromise &) {
}
```

## Future 单消费者和移动语义

`Future<T>` 是 move-only、单消费者对象。它不是可以反复读取的共享句柄。下面这些操作都会消费源 future：

- `get()` / `getFor()` / `getUntil()` 成功取到结果时。
- `thenValue()` / `thenTry()` / `thenError()` / `ensure()` 注册 continuation 时。
- `within()` / `delayed()` / `onTimeout()` 这类 combinator 接管源 future 时。
- `collectAll()` / `collect()` / `collectAny()` 接管输入 future 列表时。

消费后，原 future 进入 invalid 状态，再使用会抛 `async::FutureInvalid`。

```cpp
auto source = async::makeReadyFuture(1);

auto next = std::move(source).thenValue([](int value) {
    return value + 1;
});

// source.valid() == false
// source.get(); // 会抛 async::FutureInvalid

const int value = next.get(); // 2
```

这也是为什么大多数链式 API 都需要 `std::move(source)`。如果直接写成临时对象链式调用，则不需要手动 move：

```cpp
auto future = async::makeReadyFuture(1)
    .thenValue([](int value) {
        return value + 1;
    });
```

不要先保存一个 future，再同时把它交给两个 consumer：

```cpp
auto source = async::makeReadyFuture(42);

auto first = std::move(source).thenValue([](int value) {
    return value;
});

// 错误：source 已经被 first 消费。
// auto second = std::move(source).thenValue([](int value) {
//     return value;
// });
```

多个 consumer 需要同一个结果时，用 `FutureSplitter<T>`，前提是 `T` 可拷贝。

```cpp
async::FutureSplitter<int> splitter(async::makeReadyFuture(42));

auto first = splitter.getFuture();
auto second = splitter.getFuture();
```

`waitFor()` / `waitUntil()` 只是观察 ready 状态，不消费 future。

```cpp
auto future = async::makeReadyFuture(42);

if (future.waitFor(std::chrono::milliseconds(1))) {
    const int value = future.get(); // get() 这里才消费
}
```

## Ready 和 Failed Future

直接构造成功结果：

```cpp
auto future = async::makeReadyFuture(42);
```

直接构造失败结果：

```cpp
auto future = async::makeExceptionFuture<int>(
    std::make_exception_ptr(std::runtime_error("load failed")));

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

## Executor

executor 只回答“任务在哪运行”。

当前内置 executor：

- `InlineExecutor`：立即在当前调用栈执行。
- `ThreadExecutor`：每个任务创建 detached thread。
- `ThreadPoolExecutor`：固定 worker 池。
- `ManualExecutor`：测试手动 drain。
- `SerialExecutor`：包装另一个 executor，保证 FIFO 且不内联重入。
- `SingleThreadExecutor`：专属 worker thread，适合 runtime/GL 线程亲和对象。

用 `async::async()` 投递普通 callable：

```cpp
auto executor = std::make_shared<async::ThreadPoolExecutor>(4);

auto future = async::async(executor, []() {
    return 42;
});

const int value = future.get();
```

返回 `void` 会被提升为 `Future<Unit>`：

```cpp
auto future = async::async(async::InlineExecutor::instance(), []() {
    // do work
});

future.get();
```

返回 `Future<T>` 会自动 flatten：

```cpp
auto future = async::async(async::InlineExecutor::instance(), []() {
    return async::makeReadyFuture(42);
});

const int value = future.get(); // int，不是 Future<int>
```

如果 executor 拒绝任务，future 会以 `async::ExecutorRejected` 完成，不会永久 pending。

## 返回值和 Flatten 规则

这些入口使用同一套返回值规则：

- `async::async(executor, func)`
- `Future<T>::thenValue(func)`
- `Future<T>::thenTry(func)`
- `Future<T>::thenError(func)`
- `execution::TaskScheduler::submit(func)`
- `execution::RuntimeExecutor::submit(func)`

规则如下：

| callable 返回值 | 输出 future 类型 | 完成语义 |
| --- | --- | --- |
| `T` | `Future<T>` | 用返回值完成 |
| `void` | `Future<Unit>` | callable 正常返回后完成 |
| `Future<T>` | `Future<T>` | flatten，等待 inner future 完成 |
| 抛异常 | `Future<T>` | 输出 future 保存该异常 |
| invalid `Future<T>` | `Future<T>` | 输出 future 保存 `async::FutureInvalid` |

普通值：

```cpp
auto future = async::makeReadyFuture(1)
    .thenValue([](int value) {
        return value + 1;
    });

const int value = future.get(); // 2
```

`void` 被提升成 `Future<Unit>`：

```cpp
auto future = async::makeReadyFuture()
    .thenValue([]() {
        // do work
    });

future.get();
```

返回 inner future 会 flatten，而不是得到 `Future<Future<T>>`：

```cpp
auto future = async::makeReadyFuture(1)
    .thenValue([](int value) {
        return async::makeReadyFuture(value + 1);
    });

const int value = future.get(); // 2
```

如果 inner future 失败，外层 future 也失败：

```cpp
auto future = async::makeReadyFuture(1)
    .thenValue([](int) {
        return async::makeExceptionFuture<int>(
            std::make_exception_ptr(std::runtime_error("inner failed")));
    });

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

不要返回 invalid future。需要拒绝任务时，返回 failed future。

```cpp
auto rejected = async::makeExceptionFuture<int>(
    std::make_exception_ptr(execution::TaskRejected()));
```

## via 和 Continuation

`via(executor)` 设置当前 future 的下一次 continuation executor。它的语义是“下一个 callback 在哪里跑”，不是“把整条链永久绑定到这个 executor”。

核心规则：

- `via(executor)` 会消费当前 future 并返回同一个 future 的移动结果，后续通常紧跟 `thenValue()` / `thenTry()`。
- `via()` 只影响当前 future 上注册的下一次 callback。
- callback 运行后，如果返回普通值或 `void`，下游 future 通常会在这个 callback 完成的线程内继续完成。
- callback 如果返回 pending `Future<T>`，外层 future 会等待 inner future；后续 continuation 会跟随 inner future 的完成线程，除非你再次 `.via(executor)`。
- 没有设置 executor 时，callback 通过 `InlineExecutor` 执行，通常发生在上游完成或注册 callback 的调用栈上。
- executor 拒绝调度时，下游 future 会得到 `async::ExecutorRejected`。

固定下一次 callback 到线程池：

```cpp
auto executor = std::make_shared<async::ThreadPoolExecutor>(2);

auto future = async::makeReadyFuture(1)
    .via(executor)
    .thenValue([](int value) {
        return value + 1;
    });

const int value = future.get(); // 2
```

如果需要多个异步边界，显式多次 `.via()`：

```cpp
auto cpuExecutor = std::make_shared<async::ThreadPoolExecutor>(4);
auto ioExecutor = std::make_shared<async::SingleThreadExecutor>("io");

auto future = async::makeReadyFuture(std::string("input"))
    .via(cpuExecutor)
    .thenValue([](std::string text) {
        return parseText(std::move(text));
    })
    .via(ioExecutor)
    .thenValue([](ParsedData data) {
        return writeData(std::move(data));
    });
```

如果中间 callback 返回一个 pending inner future，下游不会自动回到前一个 executor：

```cpp
auto cpuExecutor = std::make_shared<async::ThreadPoolExecutor>(4);
auto ioExecutor = std::make_shared<async::SingleThreadExecutor>("io");

auto future = async::makeReadyFuture(std::string("path"))
    .via(ioExecutor)
    .thenValue([](std::string path) {
        return asyncReadFile(std::move(path)); // 返回 Future<std::string>
    })
    .thenValue([](std::string bytes) {
        // 这个 callback 跟随 asyncReadFile() 的完成线程。
        return bytes.size();
    });

auto pinned = async::makeReadyFuture(std::string("path"))
    .via(ioExecutor)
    .thenValue([](std::string path) {
        return asyncReadFile(std::move(path));
    })
    .via(cpuExecutor)
    .thenValue([](std::string bytes) {
        // 这里明确切回 cpuExecutor。
        return bytes.size();
    });
```

`ManualExecutor` 常用于测试，因为只有调用 `drain()` 后任务才会执行：

```cpp
auto executor = std::make_shared<async::ManualExecutor>();

auto future = async::makeReadyFuture(1)
    .via(executor)
    .thenValue([](int value) {
        return value + 1;
    });

// callback 已入队，但尚未执行。
executor->drain();

const int value = future.get(); // 2
```

`thenValue()` 只处理成功值；上游异常会原样跳过它。

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .thenValue([](int value) {
        return value + 1; // 不会执行
    });
```

## thenTry 和 Try

`thenTry()` 同时观察成功和失败。它的 handler 接收 `async::Try<T> &&`：

- `result.hasValue()` 表示有值。
- `result.hasException()` 表示保存了异常。
- `std::move(result).value()` 会取出值；如果保存的是异常，会重新抛出该异常。
- `result.exception()` 返回 `std::exception_ptr`，适合转发或记录。

成功路径：

```cpp
auto future = async::makeReadyFuture(21)
    .thenTry([](async::Try<int> &&result) {
        if (result.hasException()) {
            return -1;
        }
        return std::move(result).value() * 2;
    });

const int value = future.get(); // 42
```

失败路径中可以检查异常并恢复：

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .thenTry([](async::Try<int> &&result) {
        if (result.hasException()) {
            return 0;
        }
        return std::move(result).value();
    });

const int value = future.get(); // 0
```

也可以不恢复，直接把原 `Try<T>` 传下去：

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .thenTry([](async::Try<int> &&result) {
        return std::move(result).value(); // 重新抛出原异常
    });

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

需要按异常类型分支时，用 `exception()` + `std::rethrow_exception()`：

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .thenTry([](async::Try<int> &&result) {
        if (!result.hasException()) {
            return std::move(result).value();
        }

        try {
            std::rethrow_exception(result.exception());
        } catch (const std::runtime_error &) {
            return 0;
        } catch (...) {
            std::rethrow_exception(result.exception());
        }
    });
```

`Future<Unit>` 的 `thenTry()` 接收 `Try<Unit>`。成功时 `value()` 返回 `Unit`，通常只用于检查是否有异常：

```cpp
auto future = async::makeReadyFuture()
    .thenTry([](async::Try<async::Unit> &&result) {
        if (result.hasException()) {
            return false;
        }
        return true;
    });
```

## thenError

`thenError()` 是异常恢复 continuation。它只在上游 future 失败时执行；如果上游成功，结果会原样传递，handler 不会被调用。

最常见写法是按异常类型恢复：

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .thenError<std::runtime_error>([](const std::runtime_error &) {
        return 0;
    });

const int value = future.get(); // 0
```

匹配规则：

- `thenError<E>()` 会重新抛出上游异常并尝试捕获 `const E &`。
- 类型匹配时运行 handler。
- 类型不匹配时不运行 handler，原异常继续向下游传播。
- handler 抛出的新异常会替换原异常。
- handler 可以返回 `T` 或 `Future<T>`；返回 `Future<T>` 时会 flatten。
- 对 `Future<Unit>`，handler 可以返回 `void`。

类型不匹配时，原异常会继续保留：

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .thenError<std::logic_error>([](const std::logic_error &) {
        return 0; // 不会执行，因为 runtime_error 不是 logic_error
    });

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

handler 返回 future 时，外层 future 会等待它完成：

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .thenError<std::runtime_error>([](const std::runtime_error &) {
        return async::makeReadyFuture(42);
    });

const int value = future.get(); // 42
```

handler 自己抛异常时，下游观察到 handler 的异常：

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("original")))
    .thenError<std::runtime_error>([](const std::runtime_error &) -> int {
        throw std::logic_error("recovery failed");
    });

try {
    future.get();
} catch (const std::logic_error &) {
}
```

不带类型的 `thenError()` 接收 `std::exception_ptr`，适合统一兜底恢复或转成日志。

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .thenError([](std::exception_ptr) {
        return 0;
    });
```

如果要先处理特定异常，再做兜底恢复，可以连续接多个 `thenError()`：

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .thenError<std::logic_error>([](const std::logic_error &) {
        return 1;
    })
    .thenError([](std::exception_ptr) {
        return 0;
    });

const int value = future.get(); // 0
```

## ensure

`ensure()` 是同步清理钩子，类似 finally：

- 上游成功时会执行。
- 上游失败时也会执行。
- handler 不接收上游值或异常。
- handler 成功返回后，原成功值或原异常继续向下游传递。
- handler 的普通返回值会被忽略。
- handler 抛异常时，下游 future 以 handler 的异常完成。

成功路径中执行清理：

```cpp
bool cleaned = false;

auto future = async::makeReadyFuture(42)
    .ensure([&]() {
        cleaned = true;
    });

const int value = future.get();
```

失败路径中也会执行清理，并保留原异常：

```cpp
bool cleaned = false;

auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed")))
    .ensure([&]() {
        cleaned = true;
    });

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

如果 `ensure()` 自己抛异常，下游 future 以 `ensure()` 的异常完成。这个异常会替换原结果，即使上游本来已经失败。

```cpp
auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("original")))
    .ensure([]() {
        throw std::logic_error("cleanup failed");
    });

try {
    future.get();
} catch (const std::logic_error &) {
}
```

`ensure()` 当前是同步清理钩子，不会等待 handler 返回的 future。需要异步清理时，应显式写成 `thenTry()` 链，把原结果保存下来，再等待清理 future。

```cpp
auto future = async::makeReadyFuture(42)
    .thenTry([](async::Try<int> &&result) {
        auto saved = std::make_shared<async::Try<int>>(std::move(result));
        return async::sleepFor(std::chrono::milliseconds(10))
            .thenValue([saved]() mutable {
                return std::move(*saved).value();
            });
    });
```

## 异常传播

用户 callable 抛出的异常会被捕获进 future。

```cpp
auto future = async::async(async::InlineExecutor::instance(), []() -> int {
    throw std::runtime_error("task failed");
});

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

continuation 抛异常也会传播到下游。

```cpp
auto future = async::makeReadyFuture(42)
    .thenValue([](int) -> int {
        throw std::runtime_error("continuation failed");
    });

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

如果 callable 返回 failed future，外层 future 会等待它并传播其异常。

```cpp
auto future = async::async(async::InlineExecutor::instance(), []() {
    return async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("inner failed")));
});

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

返回 invalid future 是调用方错误，会变成 `async::FutureInvalid`。

```cpp
auto future = async::async(async::InlineExecutor::instance(), []() {
    return async::Future<int>();
});

try {
    future.get();
} catch (const async::FutureInvalid &) {
}
```

## TaskScheduler

`TaskScheduler` 是项目级 submit helper。它持有一个 executor，把 callable 投递过去，并返回 `Future<T>`。

```cpp
#include "framework/execution/execution.h"

execution::TaskScheduler scheduler(async::InlineExecutor::instance());

auto future = scheduler.submit([]() {
    return 42;
});

const int value = future.get();
```

`TaskScheduler` 支持普通值、`void` 和 `Future<T>` flatten。

```cpp
auto future = scheduler.submit([]() {
    return async::makeReadyFuture(42);
});

const int value = future.get();
```

scheduler shutdown 后拒绝新任务，返回 `execution::ExecutorShutdown`。

```cpp
scheduler.shutdown();

auto future = scheduler.submit([]() {
    return 42;
});

try {
    future.get();
} catch (const execution::ExecutorShutdown &) {
}
```

缺少 executor 时返回 `execution::TaskRejected`。

```cpp
execution::TaskScheduler scheduler(nullptr);

auto future = scheduler.submit([]() {
    return 42;
});

try {
    future.get();
} catch (const execution::TaskRejected &) {
}
```

## RuntimeExecutor

`RuntimeExecutor` 拥有一个 `IRuntime`，并把 `initialize()`、`submit()`、`shutdown()` 固定到同一条 `SingleThreadExecutor`。

生命周期：

```text
create RuntimeExecutor
  -> initialize().get()
  -> submit(...) / submitBlocking(...)
  -> shutdown().get()
```

线程边界：

- `IRuntime::initialize()` 在 runtime executor 线程执行。
- `submit(F)` 中的 `F(IRuntime&)` 在 runtime executor 线程执行。
- `shutdown()` 中的 runtime shutdown 和释放在 runtime executor 线程执行。
- `RuntimeExecutor` 默认创建 `SingleThreadExecutor("runtime")`，也可以构造时注入 executor。
- `isOnRuntimeThread()` 可用于判断当前是否已经在 runtime executor 线程。

项目约束：

- `RuntimeExecutor` 只负责 runtime 线程亲和和顺序执行，不理解 photo editor 业务。
- photo editor 主路径中，每个 GL task 内最多调用一个 `photo_editor_*` 函数。
- latest-only、process-again、destroy 优先级和 generation 防护属于 `PhotoEditorHandleActor`。

```cpp
#include "framework/execution/runtime_executor.h"
#include "framework/execution/runtime_scope.h"

auto runtimeExecutor =
    std::make_unique<execution::RuntimeExecutor>(backend->createRuntime());

runtimeExecutor->initialize().get();

auto future = runtimeExecutor->submit([](IRuntime &runtime) {
    QString error;
    RuntimeScope scope(&runtime, &error);
    if (!scope.ok()) {
        throw std::runtime_error(error.toStdString());
    }

    // 在这里调用需要 runtime/GL context 的单步函数。
    return 42;
});

const int value = future.get();

runtimeExecutor->shutdown().get();
```

`submit()` 的 callable 类型是 `F(IRuntime&)`，返回值同样支持普通值、`void`、`Future<T>` flatten。

```cpp
auto future = runtimeExecutor->submit([](IRuntime &) {
    return async::makeReadyFuture(42);
});

const int value = future.get();
```

runtime 初始化失败会变成 `execution::RuntimeInitializeFailed`。

```cpp
try {
    runtimeExecutor->initialize().get();
} catch (const execution::RuntimeInitializeFailed &) {
}
```

runtime 不可用时，提交会变成 `execution::RuntimeUnavailable`。

shutdown 后提交会变成 `execution::ExecutorShutdown`。

```cpp
runtimeExecutor->shutdown().get();

auto future = runtimeExecutor->submit([](IRuntime &) {
    return 42;
});

try {
    future.get();
} catch (const execution::ExecutorShutdown &) {
}
```

`shutdown()` 是幂等的，但正常业务应显式调用并观察结果。析构函数里的 shutdown 只是兜底，会吞掉异常。

```cpp
try {
    runtimeExecutor->shutdown().get();
} catch (const std::exception &ex) {
    reportShutdownFailure(QString::fromStdString(ex.what()));
}
```

不要在 `RuntimeExecutor` task 内混入业务流控。task 只应执行需要 runtime/GL context 的单步操作：

```cpp
auto future = runtimeExecutor->submit([handle, parameters](IRuntime &runtime) {
    QString error;
    RuntimeScope scope(&runtime, &error);
    if (!scope.ok()) {
        throw std::runtime_error(error.toStdString());
    }

    if (!photo_editor_set_opcode(handle, parameters, &error)) {
        throw std::runtime_error(error.toStdString());
    }
});
```

连续业务请求应该在 actor 内合并或折叠，再投递单步 task，而不是让 `RuntimeExecutor` 识别业务类型。

## submitBlocking

`submitBlocking()` 用于必须同步完成的路径。调用方不在 runtime 线程时，它等价于 `submit(F).get()`。

```cpp
const int value = runtimeExecutor->submitBlocking([](IRuntime &runtime) {
    QString error;
    RuntimeScope scope(&runtime, &error);
    if (!scope.ok()) {
        throw std::runtime_error(error.toStdString());
    }
    return 42;
});
```

如果当前已经在 runtime executor thread 上，`submitBlocking()` 会 inline 执行，避免自己等待自己。

```cpp
auto future = runtimeExecutor->submit([&](IRuntime &) {
    return runtimeExecutor->submitBlocking([](IRuntime &) {
        return 42;
    });
});
```

`submitBlocking()` 也支持普通值、`void`、`Future<T>` flatten。调用方需要注意两个风险：

- 从非 runtime 线程调用时会阻塞当前线程，直到 task 完成。
- 从 runtime 线程调用时会 inline 执行；如果 callable 返回一个仍依赖同一 runtime executor 才能完成的 pending future，业务层仍可能自等待。

错误示例：

```cpp
auto future = runtimeExecutor->submit([&](IRuntime &) {
    return runtimeExecutor->submitBlocking([&](IRuntime &) {
        // 返回的 inner future 还需要 runtime executor 后续任务完成。
        return runtimeExecutor->submit([](IRuntime &) {
            return 42;
        });
    });
});
```

上面的代码在 runtime 线程 inline 分支里等待同一 executor 的后续 task，容易造成自等待。应改成普通异步链路，或者避免在 runtime 线程上同步等待。

## Timeout 和 Delay

本库有两类 timeout，用途不同：

- `getFor()` / `getUntil()`：限制当前调用线程等待 `get()` 的时间。
- `within()` / `onTimeout()`：创建一个带超时竞争的新 future。

| 接口 | 返回值 | 是否消费原 future | 是否通知 producer | 适合场景 |
| --- | --- | --- | --- | --- |
| `getFor()` / `getUntil()` | 直接返回 `T` 或抛异常 | 只有成功取到结果时消费；超时不消费 | 否 | 同步等待某个 future，但不想无限阻塞当前线程 |
| `within()` | 返回新的 `Future<T>` | 是 | timeout 胜出时发送 interrupt | 给异步链路加超时边界 |
| `onTimeout()` | 返回新的 `Future<T>` | 是 | timeout 胜出时发送 interrupt | 给异步链路加 fallback |

`getFor()` / `getUntil()` 不改变 producer，也不会给 producer 发送 interrupt。超时时抛 `async::FutureTimeout`，但不消费原 future；调用方以后仍可继续等待或再调用 `get()`。

```cpp
auto future = async::sleepFor(std::chrono::seconds(1))
    .thenValue([]() {
        return 42;
    });

try {
    const int value = future.getFor(std::chrono::milliseconds(10));
} catch (const async::FutureTimeout &) {
    // future 未被消费，仍可继续等待。
}

const int value = future.get(); // 仍然可以继续等待最终结果
```

`within()` 返回一个新的 future，让原 future 和 timer 竞争：

- 原 future 先完成：输出 future 得到原成功值或原异常，timer 被取消。
- timer 先完成：输出 future 得到 `async::FutureTimeout`。
- timer 胜出时，会向原 producer 发送 `FutureTimeout` interrupt。
- interrupt 只是通知，不会强杀底层任务。
- 原 future 会被 `within()` 消费；之后只能使用 `within()` 返回的新 future。

```cpp
auto future = async::within(
    async::sleepFor(std::chrono::seconds(1)).thenValue([]() {
        return 42;
    }),
    std::chrono::milliseconds(10));

try {
    future.get();
} catch (const async::FutureTimeout &) {
}
```

`within()` 适合给一段异步链增加超时边界。下游看到的是新的输出 future，而不是原 future。

```cpp
auto future = async::within(
        async::async(std::make_shared<async::ThreadExecutor>(), []() {
            return slowRead();
        }),
        std::chrono::milliseconds(100))
    .thenValue([](std::string bytes) {
        return parseBytes(bytes);
    });
```

如果上游先失败，`within()` 不会把失败改成 timeout，而是原样传播上游异常：

```cpp
auto future = async::within(
    async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("read failed"))),
    std::chrono::seconds(1));

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

`onTimeout()` 是 `within(...).thenError<FutureTimeout>(...)` 的便捷封装，可以只恢复 timeout，不吞掉其它异常。

```cpp
auto future = async::onTimeout(
    async::sleepFor(std::chrono::seconds(1)).thenValue([]() {
        return 42;
    }),
    std::chrono::milliseconds(10),
    []() {
        return -1;
    });

const int value = future.get(); // -1
```

非 timeout 异常不会被 `onTimeout()` 的 fallback 处理：

```cpp
auto future = async::onTimeout(
    async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("read failed"))),
    std::chrono::seconds(1),
    []() {
        return -1;
    });

try {
    future.get();
} catch (const std::runtime_error &) {
}
```

`onTimeout()` 的 fallback 也可以返回 `Future<T>`：

```cpp
auto future = async::onTimeout(
    async::sleepFor(std::chrono::seconds(1)).thenValue([]() {
        return 42;
    }),
    std::chrono::milliseconds(10),
    []() {
        return async::makeReadyFuture(-1);
    });
```

`delayed()` 只延迟交付，不改变值或异常。

```cpp
auto future = async::delayed(
    async::makeReadyFuture(42),
    std::chrono::milliseconds(50));
```

## 协作式取消

取消接口是协作式 interrupt 协议，不是强制终止：

- `future.cancel()` 等价于向 producer 发送 `async::FutureCancelled`。
- `future.raise(exception)` 可以发送任意 interrupt 异常。
- `future.interruptHandle()` 可以拿到一个可复制的小句柄，在 future 被移动后继续发送 interrupt。
- interrupt handler 在 producer 侧通过 `Promise::setInterruptHandler()` 注册。
- interrupt handler 是否完成 promise、如何完成 promise，完全由 producer 决定。
- 如果没有注册 handler，interrupt 会被保存，后续注册 handler 时会投递。
- 如果 future 已经完成，再发送 interrupt 不会改变结果。

最简单的 producer 可以把 cancel 转成 failed future：

```cpp
auto promise = std::make_shared<async::Promise<int>>();
auto future = promise->getFuture();

promise->setInterruptHandler([promise](std::exception_ptr interrupt) {
    promise->setException(std::move(interrupt));
});

future.cancel();

try {
    future.get();
} catch (const async::FutureCancelled &) {
}
```

更常见的异步任务会在 interrupt handler 里设置一个停止标志，然后由 worker 自己决定何时完成 promise。这样可以安全释放资源，而不是从外部强杀线程。

```cpp
struct WorkerState
{
    std::atomic<bool> cancelled { false };
};

auto state = std::make_shared<WorkerState>();
auto promise = std::make_shared<async::Promise<int>>();
auto future = promise->getFuture();

promise->setInterruptHandler([state](std::exception_ptr) {
    state->cancelled.store(true);
});

std::thread([state, promise]() {
    for (int i = 0; i < 100; ++i) {
        if (state->cancelled.load()) {
            promise->setException(std::make_exception_ptr(async::FutureCancelled()));
            return;
        }
        doOneStep();
    }
    promise->setValue(42);
}).detach();

future.cancel();
```

`within()` 使用同一套 interrupt 机制。timeout 胜出时，输出 future 已经完成为 `FutureTimeout`，同时原 producer 会收到 `FutureTimeout` interrupt。如果 producer 不处理 interrupt，底层任务仍可能继续跑到自然结束。

```cpp
auto promise = std::make_shared<async::Promise<int>>();
auto source = promise->getFuture();

promise->setInterruptHandler([promise](std::exception_ptr interrupt) {
    try {
        std::rethrow_exception(interrupt);
    } catch (const async::FutureTimeout &) {
        // 可选择停止底层操作，并用同样的 timeout 完成原 promise。
        promise->setException(std::make_exception_ptr(async::FutureTimeout()));
    } catch (...) {
        promise->setException(std::current_exception());
    }
});

auto timed = async::within(std::move(source), std::chrono::milliseconds(10));
```

也可以保存 `InterruptHandle`，在 future 被移动后继续通知 producer。

```cpp
async::Promise<int> promise;
auto future = promise.getFuture();
auto interrupt = future.interruptHandle();

interrupt.raise(std::make_exception_ptr(async::FutureCancelled()));
```

注意这个示例只发送 interrupt。如果没有 producer 注册 handler 并完成 promise，`future.get()` 仍会等待。

## collectAll / collect / collectAny

`collectAll()` 等待所有输入完成，返回每个输入的 `Try<T>`，不会因单个失败提前失败。

规则：

- 输入 future 列表会被消费。
- 空列表返回 ready future，结果为空 vector。
- 任一输入 future invalid 时，输出 future 以 `async::FutureInvalid` 失败。
- 每个输入无论成功或失败，都在结果 vector 中占一个位置。
- 输出结果顺序和输入顺序一致，不按完成时间排序。

```cpp
std::vector<async::Future<int>> futures;
futures.push_back(async::makeReadyFuture(1));
futures.push_back(async::makeExceptionFuture<int>(
    std::make_exception_ptr(std::runtime_error("failed"))));

auto all = async::collectAll(std::move(futures));
std::vector<async::Try<int>> results = all.get();

const int first = results[0].value();
const bool secondFailed = results[1].hasException();
```

`collect()` 只返回值列表；如果任一输入失败，读取结果时会抛出对应异常。

`collect()` 基于 `collectAll()`，因此也会等待所有输入完成。区别是它会把 `Try<T>` 转成 `T`，转换过程中任一失败结果都会重新抛出。

```cpp
std::vector<async::Future<int>> futures;
futures.push_back(async::makeReadyFuture(1));
futures.push_back(async::makeReadyFuture(2));

std::vector<int> values = async::collect(std::move(futures)).get();
```

失败示例：

```cpp
std::vector<async::Future<int>> futures;
futures.push_back(async::makeReadyFuture(1));
futures.push_back(async::makeExceptionFuture<int>(
    std::make_exception_ptr(std::runtime_error("failed"))));

auto collected = async::collect(std::move(futures));

try {
    std::vector<int> values = collected.get();
} catch (const std::runtime_error &) {
}
```

`collectAny()` 返回第一个完成输入的 index 和 `Try<T>`。

规则：

- 第一个成功完成或失败完成的输入都会胜出。
- 返回的 index 是输入 vector 中的位置。
- 返回的 `Try<T>` 可能是值，也可能是异常。
- 不会取消其它尚未完成的输入。

```cpp
std::vector<async::Future<int>> futures;
futures.push_back(async::sleepFor(std::chrono::milliseconds(20)).thenValue([]() {
    return 1;
}));
futures.push_back(async::makeReadyFuture(2));

auto result = async::collectAny(std::move(futures)).get();
const std::size_t index = result.first;
const int value = std::move(result.second).value();
```

如果第一个完成的是失败结果，`Try<T>` 会保存异常：

```cpp
std::vector<async::Future<int>> futures;
futures.push_back(async::makeExceptionFuture<int>(
    std::make_exception_ptr(std::runtime_error("failed first"))));
futures.push_back(async::sleepFor(std::chrono::milliseconds(20)).thenValue([]() {
    return 42;
}));

auto result = async::collectAny(std::move(futures)).get();
if (result.second.hasException()) {
    try {
        std::move(result.second).value();
    } catch (const std::runtime_error &) {
    }
}
```

## FutureSplitter

`Future<T>` 是单消费者。如果多个 consumer 都需要同一个结果，并且 `T` 可拷贝，可以使用 `FutureSplitter<T>`。

规则：

- `FutureSplitter<T>` 会消费源 future。
- `T` 必须可拷贝，因为每个 consumer 都会得到一份结果副本。
- 源 future 成功时，每个 waiter 都得到同样的值。
- 源 future 失败时，每个 waiter 都得到同样的异常。
- 源 future 完成后再调用 `getFuture()`，会立即得到已完成的新 future。

```cpp
async::FutureSplitter<int> splitter(async::makeReadyFuture(42));

auto first = splitter.getFuture();
auto second = splitter.getFuture();

const int a = first.get();
const int b = second.get();
```

失败结果也会广播给所有 consumer：

```cpp
async::FutureSplitter<int> splitter(
    async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("failed"))));

auto first = splitter.getFuture();
auto second = splitter.getFuture();

try {
    first.get();
} catch (const std::runtime_error &) {
}

try {
    second.get();
} catch (const std::runtime_error &) {
}
```

## 业务 actor 和 execution 的分界

execution 层不提供 lane，也不理解业务请求类型。连续业务请求的策略放在 actor/mailbox 内：

```cpp
void PhotoEditorHandleActor::setOpcode(ImageEffectParameters parameters)
{
    bool shouldPost = false;
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        m_latestParameters = parameters;
        generation = m_generation;
        if (canUseHandleLocked() && !m_opcodeTaskPending) {
            m_opcodeTaskPending = true;
            shouldPost = true;
        }
    }

    if (shouldPost) {
        postSetOpcodeTask(generation);
    }
}
```

上面这种 latest-only 语义属于 photo editor 业务。`RuntimeExecutor` 只负责把 `postSetOpcodeTask()` 中的 GL task 顺序投递到 runtime 线程。

## 常见反模式

- 不要把 latest-only、merge、drop 写进 `RuntimeExecutor`。
- 不要在 execution 层引入 Qt widget、OpenGL 业务结果或 photo editor 类型。
- 不要在 `Future<T>` 被 `get()`、`thenValue()`、`thenTry()` 消费后继续使用它。
- 不要返回 invalid future；需要拒绝时返回 failed future。
- 不要在 runtime 线程等待一个仍需要同一 runtime executor 才能完成的 pending future。
- 不要依赖 timeout 强杀底层任务；timeout 只是完成外层 future 并向 producer 发送协作式 interrupt。
