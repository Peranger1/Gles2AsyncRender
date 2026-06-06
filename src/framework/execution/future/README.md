# Async Future

This directory contains a standalone C++17 promise/future model. It depends only
on the C++ standard library and is intentionally independent from Qt, GL, and the
runtime execution layer.

## Public API

Include the aggregate header for normal use:

```cpp
#include "framework/execution/future/async_future.h"
```

Main types:

- `async::Promise<T>`: producer side, fulfilled once.
- `async::Future<T>`: move-only single-consumer future.
- `async::Try<T>`: contains either a value or `std::exception_ptr`.
- `async::Unit`: logical void value.
- `async::Executor`: continuation scheduling abstraction.
- `async::FutureSplitter<T>`: adapts one future into multiple futures.

## Semantics

- `Future<T>` is move-only and single-consumer. Calling `get()`, `thenValue()`,
  `thenTry()`, or `thenError()` consumes the source future.
- `thenValue()`, `thenTry()`, and `thenError()` support continuations returning
  values, `void`, or `Future<T>`. Returned futures are flattened.
- `Promise<T>` may be fulfilled only once. Destroying an unfulfilled promise
  completes the future with `async::BrokenPromise`.
- `Future::cancel()` and `Future::raise()` only notify the producer through
  `Promise::setInterruptHandler()`. They do not complete the future by
  themselves.
- `within()` races a future against a timeout and completes with
  `async::FutureTimeout` if the timeout wins.
- `FutureSplitter<T>` currently requires `T` to be copy constructible.
- `ManualExecutor` stores tasks until `drainOne()` or `drain()` is called,
  making it useful for deterministic tests.
- `SerialExecutor` wraps another executor and guarantees FIFO one-at-a-time
  execution over that underlying executor. Nested `add()` calls are queued
  rather than run inline.
- `SingleThreadExecutor` runs all submitted tasks FIFO on one dedicated worker
  thread. This is the intended executor for thread-affine resources such as
  EGL/GLES2 runtime objects. Nested `add()` calls are queued rather than run
  inline, so task execution does not become reentrant by default.
- `sleepFor()`, `sleepUntil()`, and `within()` use detached standard-library
  threads in this first implementation.

## Combinators

- `makeReadyFuture(value)` / `makeReadyFuture()`
- `makeExceptionFuture<T>(exception)`
- `collectAll(vector<Future<T>>)` returns `Future<vector<Try<T>>>`
- `collect(vector<Future<T>>)` returns `Future<vector<T>>`
- `collectAny(vector<Future<T>>)` returns `Future<pair<size_t, Try<T>>>`

## Tests

The standalone test runner lives in `test/future_tests.cpp`. It can be built
through CMake when `BUILD_TESTING` is enabled, or directly with the qmake project
in `test/future_tests.pro`.
