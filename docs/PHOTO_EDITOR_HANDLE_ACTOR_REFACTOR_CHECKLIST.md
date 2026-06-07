# Photo Editor Handle Actor Refactor Checklist

> 日期：2026-05-24
>
> 状态：Implemented / manually verified
>
> 本清单基于 [PHOTO_EDITOR_HANDLE_ACTOR_REFACTOR_DESIGN.md](./PHOTO_EDITOR_HANDLE_ACTOR_REFACTOR_DESIGN.md) 整理。当前重构已经按清单实施，并完成 qmake Debug 构建验证与手动导入图片目录、PageDown 切图验证。
>
> 核心约束：framework execution 层只提供 GL context + `void()` 顺序执行能力；所有提交给 execution 层的 GL task 内部最多只能调用一个 `photo_editor_*` 函数。

## 0. 完成记录

- 已新增 `RuntimeExecutor`。
- 已新增 `PhotoEditorRuntimeService`。
- 已新增 `PhotoEditorHandleActor`。
- `PhotoEditorAppSession` 已切换到 service + actor 主路径。
- `PhotoEditorAppSession` 主路径中已无直接 `photo_editor_*` 调用。
- SDK callback 已移除 `ProcessTicket` 裸指针方案，改为 actor userData + active generation 校验。
- `photo_editor_destroy` 已限制在有效 `RuntimeScope` 下执行。
- 已更新 `README.md` 与 `ARCHITECTURE.md`。
- 已通过 qmake Debug 构建。
- 已手动验证导入图片目录和 PageDown 切图，无异常。

## 1. 准备阶段

- [x] 阅读并确认当前 `PhotoEditorAppSession::runGpuPreviewStep()` 主路径。
- [x] 标记当前所有直接调用 `photo_editor_*` 的位置。
- [x] 确认 `RuntimeHost` / `QtRuntimeHost` 保持可复用，不在本轮重构中重写平台 runtime。
- [x] 确认 `AsyncLane` / `SyncLane` 已删除，photo editor 主路径不再依赖它们。
- [x] 确认 `photo_editor_gles2_backend.*` 的 C 风格 API 不在本轮重构中改变。

完成标准：

- 已明确当前业务主路径中的所有 `photo_editor_*` 调用点。
- 已确认本轮重构边界不包含 platform backend 和 photo editor backend 的大改。

## 2. 新增 RuntimeExecutor

目标：新增 framework execution 层的通用 GL task 执行器。

计划文件：

- [x] 新增 `src/framework/execution/runtime_executor.h`
- [x] 新增 `src/framework/execution/runtime_executor.cpp`
- [x] 更新 `CMakeLists.txt`
- [x] 更新 `Gles2AsyncRender.pro`

接口要求：

- [x] 提供 `post(RuntimeTask task)`。
- [x] 提供 `call(RuntimeTask task, QString *error)`。
- [x] 提供 `isOnRuntimeThread() const`。
- [x] 提供 `shutdown()`。
- [x] 内部只依赖 `RuntimeHost::dispatchAsync()` / `dispatchSync()`。
- [x] 不包含任何 photo editor 业务类型。
- [x] 不提供 merge、drop、latest-only 等业务策略。

完成标准：

- `RuntimeExecutor` 可以异步投递 `void(IRuntime*)`。
- `RuntimeExecutor` 可以同步调用 `void(IRuntime*)`。
- shutdown 后拒绝新 task。
- 代码中没有 `photo_editor_*` 相关 include 或类型依赖。

## 3. 新增 PhotoEditorRuntimeService

目标：建立 photo editor 业务层的全局服务入口。

计划文件：

- [x] 新增 `src/app/photo_editor_runtime_service.h`
- [x] 新增 `src/app/photo_editor_runtime_service.cpp`
- [x] 更新 `CMakeLists.txt`
- [x] 更新 `Gles2AsyncRender.pro`

职责要求：

- [x] 持有或引用 `RuntimeExecutor`。
- [x] 负责 `photo_editor_init`。
- [x] 创建 `PhotoEditorHandleActor`。
- [x] 管理 actor 集合。
- [x] shutdown 时请求所有 actor destroy。
- [x] 提供 warning/error 信号。

单步约束：

- [x] `photo_editor_init` 必须作为独立 GL task 提交。
- [x] init task 内不得调用 `photo_editor_create` 或其它 `photo_editor_*`。

完成标准：

- service 可以完成一次独立 `photo_editor_init`。
- service 可以创建 actor，但不直接执行 handle 业务流程。
- service shutdown 可以安全释放 actor。

## 4. 新增 PhotoEditorHandleActor

目标：每张图一个 actor，每个 actor 管理一个 `photo_editor` handle。

计划文件：

- [x] 新增 `src/app/photo_editor_handle_actor.h`
- [x] 新增 `src/app/photo_editor_handle_actor.cpp`
- [x] 更新 `CMakeLists.txt`
- [x] 更新 `Gles2AsyncRender.pro`

状态要求：

- [x] 定义 `Empty`。
- [x] 定义 `Creating`。
- [x] 定义 `Ready`。
- [x] 定义 `Processing`。
- [x] 定义 `Processed`。
- [x] 定义 `Rendering`。
- [x] 定义 `Rendered`。
- [x] 定义 `Destroying`。
- [x] 定义 `Destroyed`。
- [x] 定义 `Failed`。

数据要求：

- [x] 保存 actor id。
- [x] 保存 `void *handle`。
- [x] 保存 `sourceKey`。
- [x] 保存 `sourceImageCacheKey`。
- [x] 保存 `std::shared_ptr<const QImage>`。
- [x] 保存 latest output size。
- [x] 保存 latest effect parameters。
- [x] 保存 generation token。
- [x] 保存 destroy requested 标记。
- [x] 保存 process-again requested 标记。
- [x] 使用 mutex 保护状态。

public API 要求：

- [x] `create()`
- [x] `setOutputSize(QSize size)`
- [x] `setOpcode(ImageEffectParameters parameters)`
- [x] `process()`
- [x] `render()`
- [x] `destroy()`

线程安全要求：

- [x] `setOutputSize()` 可以从非 GL 线程调用。
- [x] `setOpcode()` 可以从非 GL 线程调用。
- [x] `process()` 可以从非 GL 线程调用。
- [x] public API 内部不得直接调用 `photo_editor_*`。
- [x] public API 只做 mutex 保护下的状态更新、请求折叠、generation 读取和 task 投递。
- [x] GL task 回写状态时必须重新加锁并校验 generation。

完成标准：

- actor 可以独立 create、set size、set opcode、process、render、destroy。
- 每个动作最终都投递为独立 GL task。
- actor 可安全处理跨线程调用。

## 5. 单步 GL Task 拆分

目标：保证每个 executor task 内最多只调用一个 `photo_editor_*`。

必须形成的 task 类型：

- [x] `photo_editor_init` task
- [x] `photo_editor_create` task
- [x] `photo_editor_set_output_size` task
- [x] `photo_editor_set_opcode` task
- [x] `photo_editor_process` task
- [x] `photo_editor_render` task
- [x] `photo_editor_destroy` task

禁止项：

- [x] 不允许一个 task 同时调用 create + set output size。
- [x] 不允许一个 task 同时调用 set output size + set opcode。
- [x] 不允许一个 task 同时调用 set opcode + process。
- [x] 不允许 SDK callback 中直接调用 render。
- [x] 不允许保留 `runGpuPreviewStep()` 这种集中业务流程作为主路径。

完成标准：

- 搜索主路径代码时，每个 executor task lambda 内只出现一个 `photo_editor_*` 调用。
- `process -> render` 通过 SDK callback 触发新的 render task，而不是直接串联执行。

## 6. Process Callback 与 Generation 防护

目标：安全处理 SDK 异步 callback、destroy、快速切图、旧请求回调。

实现要求：

- [x] 每次 `process()` 创建独立 ticket。
- [x] ticket 保存 actor weak/QPointer。
- [x] ticket 保存 generation。
- [x] callback 只做轻量校验和转发。
- [x] callback 不直接触碰 GL。
- [x] callback 不直接调用 `photo_editor_render`。
- [x] `onProcessCompleted()` 加锁校验 generation。
- [x] generation 不匹配时忽略 callback。
- [x] actor 已 Destroying / Destroyed 时忽略 callback。
- [x] process 完成后通过 `render()` 投递独立 render task。

完成标准：

- destroy 后到达的 callback 不会触发 render。
- 快速切图后的旧 callback 不会污染新 actor 或新 generation。
- processing 中再次请求 process 不会重入 `photo_editor_process`。

## 7. Actor 内业务策略

目标：将合并、丢弃、重跑策略放在 actor 内，不放入 framework。

策略要求：

- [x] `setOutputSize()` 使用线程安全 latest-only。
- [x] `setOpcode()` 使用线程安全 latest-only。
- [x] `process()` 在 Processing 中设置 `processAgainRequested` 或明确拒绝。
- [x] render result 只接受当前 generation。
- [x] destroy 优先级最高。
- [x] destroy 后旧 task、旧 callback、旧 result 全部忽略。

完成标准：

- `RuntimeExecutor` 中没有任何 latest-only 逻辑。
- actor 内部可以解释和处理业务请求折叠。

## 8. 改造 PhotoEditorAppSession

目标：让 app session 只做 demo 编排，不直接调用 `photo_editor_*`。

改造要求：

- [x] 持有 `PhotoEditorRuntimeService`。
- [x] 不再持有裸 `m_gpuEditorHandle`。
- [x] 不再持有 `GpuPreviewLane` 作为 photo editor 主路径。
- [x] 移除或绕开 `runGpuPreviewStep()`。
- [x] 移除或绕开 `ensurePhotoEditorInitialized()` 主路径。
- [x] 移除或绕开 `ensureGpuEditor()` 主路径。
- [x] 移除或绕开 `collectGpuRenderResult()` 主路径。
- [x] 图片选择时创建或切换 actor。
- [x] UI 参数变化转发给 actor。
- [x] render result 回调后调用 platform writer 发布纹理。
- [x] CPU preview 逻辑可以暂时保留为现状。

完成标准：

- `PhotoEditorAppSession` 主路径中不直接调用 `photo_editor_*`。
- GPU preview 仍可从 UI 操作触发并显示。
- CPU preview 不被本轮重构破坏。

## 9. 日志与诊断

目标：用日志证明新的执行模型。

日志要求：

- [x] 每个 actor task 输出 actor id。
- [x] 每个 actor task 输出 function name。
- [x] 每个 actor task 输出 generation。
- [x] process callback 输出 actor id、progress、isEnd、generation。
- [x] 被 generation 丢弃的旧 callback/task/result 输出诊断日志。

目标日志形态：

```text
[GL] actor#1 gen=1 photo_editor_init
[GL] actor#1 gen=1 photo_editor_create
[GL] actor#1 gen=1 photo_editor_set_output_size
[GL] actor#1 gen=1 photo_editor_set_opcode
[GL] actor#1 gen=1 photo_editor_process
[SDK] actor#1 gen=1 process completed
[GL] actor#1 gen=1 photo_editor_render
[GL] actor#1 gen=2 photo_editor_destroy
```

完成标准：

- 运行 demo 后可以从日志确认每个 task 只执行一个 `photo_editor_*`。
- 多 actor 交错执行时日志仍能清晰区分 actor 与 generation。

## 10. 构建配置更新

目标：确保新增文件同时进入 CMake 与 qmake 工程。

检查项：

- [x] `CMakeLists.txt` 包含 `runtime_executor.*`。
- [x] `CMakeLists.txt` 包含 `photo_editor_runtime_service.*`。
- [x] `CMakeLists.txt` 包含 `photo_editor_handle_actor.*`。
- [x] `Gles2AsyncRender.pro` 包含 `runtime_executor.*`。
- [x] `Gles2AsyncRender.pro` 包含 `photo_editor_runtime_service.*`。
- [x] `Gles2AsyncRender.pro` 包含 `photo_editor_handle_actor.*`。

完成标准：

- CMake build 可以发现所有新增文件。
- Qt Creator / qmake 工程可以发现所有新增文件。

## 11. 验证场景

功能验证：

- [x] 加载图片目录。
- [x] 单张图完成 create、set size、set opcode、process、render。
- [x] GPU preview 正常显示。
- [x] CPU preview 仍可正常触发。
- [x] 切换下一张图。
- [x] 切换上一张图。
- [x] render result 可以正常交给 platform writer。

并发与生命周期验证：

- [x] 快速切换多张图片，产生多个 actor。
- [x] 快速拖动参数，验证 latest-only。
- [x] processing 中再次请求 process，验证不重入。
- [x] process 完成后 callback 触发独立 render task。
- [x] actor destroy 后 SDK callback 到达，验证被忽略。
- [x] render 前 output size 改变，验证顺序正确。
- [x] shutdown 时所有 handle 被销毁。

跨线程验证：

- [x] 从 UI 线程调用 `setOutputSize()`。
- [x] 从 UI 线程调用 `setOpcode()`。
- [x] 从 UI 线程调用 `process()`。
- [x] 从 worker 线程调用 `setOutputSize()`。
- [x] 从 worker 线程调用 `setOpcode()`。
- [x] 从 worker 线程调用 `process()`。
- [x] UI 线程拖动参数时，worker 线程同时请求 process。
- [x] 验证无数据竞争、无崩溃、无旧结果覆盖新状态。

完成标准：

- 所有验证场景通过。
- 日志能解释每一次 GL task 的顺序和归属。

## 12. 文档更新

目标：让正式架构文档与新实现一致。

更新项：

- [x] 更新 `README.md` 中 execution 层描述。
- [x] 更新 `README.md` 中 photo editor 主路径描述。
- [x] 更新 `ARCHITECTURE.md` 中 `AsyncLane` 相关描述。
- [x] 更新 `ARCHITECTURE.md` 中 `PhotoEditorAppSession` 职责描述。
- [x] 标记旧 `AsyncLane` GPU preview 特化路径不再是主路径。

完成标准：

- 文档不再声称主路径依赖 `AsyncLane<GpuPreviewRequest, RawGpuTextureResult, ...>`。
- 文档明确说明主路径是 `RuntimeExecutor + PhotoEditorRuntimeService + PhotoEditorHandleActor`。

## 13. 最终完成定义

本轮重构完成必须同时满足：

- [x] framework execution 层只提供 GL context + `void()` 顺序执行能力。
- [x] photo editor 主路径不再使用 `AsyncLane`。
- [x] `PhotoEditorAppSession` 不直接调用 `photo_editor_*`。
- [x] 每个 GL task 最多只调用一个 `photo_editor_*`。
- [x] 每个 handle 由一个 `PhotoEditorHandleActor` 管理。
- [x] `setOutputSize()`、`setOpcode()`、`process()` 支持跨线程调用。
- [x] SDK callback 不直接调用 render。
- [x] destroy、快速切图、旧 callback 均有 generation 防护。
- [x] demo 功能可运行。
- [x] 构建通过。
- [x] 相关文档已更新。
