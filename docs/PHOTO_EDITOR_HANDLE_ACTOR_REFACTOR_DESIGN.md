# Photo Editor Handle Actor Refactor Design

> 日期：2026-05-24
>
> 状态：Design proposal / pending review
>
> 本文档描述下一轮 photo editor demo 与 framework execution 层的重构方案。文件名采用英文，正文采用中文，便于和现有 `docs/` 文档风格保持一致。
>
> 本文档只描述待审查方案，不代表当前代码已经实现。若本文内容与当前代码不一致，以当前代码为准。

## 1. 背景

当前实现中，`PhotoEditorAppSession::runGpuPreviewStep()` 将一次 GPU 预览请求打包成一段较大的业务流程：

```text
ensure init
ensure/create handle
photo_editor_set_output_size
photo_editor_set_opcode
photo_editor_process
SDK callback
photo_editor_render
```

这条路径可以验证 GPU preview 业务闭环，但无法验证 framework execution 层是否能够稳定承载“任意单个需要 GL 上下文的 `photo_editor_*` 函数调用”。

历史 `AsyncLane` 抽象更接近 `process + render` 这种带 SDK 回调的业务特化 lane，而不是通用 GL 线程事件执行器；该实现当前已经删除。随着 `photo_editor_*` 函数增加，真正关键的问题不是 framework 如何理解业务流程，而是业务层如何把每个 `photo_editor_*` 函数打包成单个 `void()` 调用，并提交到 GL 线程顺序执行。

## 2. 目标

本轮重构目标如下：

- framework execution 层只提供 GL 上下文环境与 `void()` 顺序调用能力。
- framework execution 层不关心 photo editor 业务语义。
- framework execution 层不负责合并、丢弃、重试、严格串行业务策略。
- photo editor 业务层负责把每个 `photo_editor_*` 调用封装成单步 GL task。
- demo 中禁止把多个 `photo_editor_*` 调用集中打包在一个函数或一个 GL task 中执行。
- 每个提交到 execution 层的 GL task 内部最多只能调用一个 `photo_editor_*` 函数。
- 每个 handle 对应一张图，并由独立业务对象管理其生命周期和状态。
- `PhotoEditorHandleActor::setOutputSize()`、`setOpcode()`、`process()` 允许从其它线程调用，并且必须是线程安全入口。

## 3. 非目标

本方案不试图在 framework 层提供以下能力：

- 识别 `photo_editor_*` 函数含义。
- 判断某个 handle 是否处于 processing。
- 自动合并业务请求。
- 自动丢弃过期业务结果。
- 自动串联 `process -> render`。
- 替业务层管理 SDK callback 生命周期。

这些能力都应该属于 photo editor 业务层。

## 4. 总体方案

采用“framework GL executor + photo editor runtime service + per-handle actor”的三层结构。

```text
PhotoEditorAppSession
  |
  v
PhotoEditorRuntimeService
  |
  v
PhotoEditorHandleActor  <--- one actor per image/handle
  |
  v
RuntimeExecutor
  |
  v
RuntimeHost / QtRuntimeHost
  |
  v
IRuntime + current GL context
```

其中：

- `RuntimeExecutor` 属于 framework execution 层，只负责顺序执行 `void(IRuntime*)`。
- `PhotoEditorRuntimeService` 属于业务层，负责 photo editor 全局初始化和 actor 创建。
- `PhotoEditorHandleActor` 属于业务层，一个 actor 管理一张图对应的 `void *handle`，并提供线程安全的业务调用入口。
- `PhotoEditorAppSession` 只做 demo 编排，不再直接调用 `photo_editor_*`。

## 5. RuntimeExecutor

`RuntimeExecutor` 是 framework 层的核心抽象。它比历史 `AsyncLane` 更底层、更通用。

建议接口：

```cpp
using RuntimeTask = std::function<void(IRuntime *)>;

class RuntimeExecutor final
{
public:
    explicit RuntimeExecutor(RuntimeHost *host);

    SubmitResult post(RuntimeTask task);
    bool call(RuntimeTask task, QString *error);
    bool isOnRuntimeThread() const;
    void shutdown();
};
```

设计约束：

- `post()` 使用 GL 线程异步执行 task。
- `call()` 使用 GL 线程同步执行 task。
- executor 不知道 task 内部执行什么业务函数。
- executor 不知道 task 是否是 create、process、render 或 destroy。
- executor 不提供 merge/drop/latest-only 策略。
- executor 不持有 photo editor handle。
- executor 不直接创建 `RuntimeScope` 也可以，但建议由 task 内部创建 `RuntimeScope`，这样业务层可以清楚地控制错误记录与函数边界。

framework 层只保证：

```text
提交顺序 == GL 线程执行顺序
每个 task 执行时可获得 IRuntime*
task 执行期间可以进入 GL context
shutdown 后拒绝新 task
```

## 6. PhotoEditorRuntimeService

`PhotoEditorRuntimeService` 是 photo editor 业务层的全局入口。它持有 `RuntimeExecutor`，负责 `photo_editor_init` 和 actor 生命周期管理。

建议职责：

- 创建并持有 `RuntimeExecutor`。
- 确保 `photo_editor_init` 只执行一次。
- 为每张图创建一个 `PhotoEditorHandleActor`。
- 在 shutdown 时通知所有 actor 销毁 handle。
- 提供全局日志和错误上报桥接。

建议接口：

```cpp
class PhotoEditorRuntimeService final : public QObject
{
    Q_OBJECT

public:
    explicit PhotoEditorRuntimeService(RuntimeHost *host, QObject *parent = nullptr);

    void initialize();
    std::shared_ptr<PhotoEditorHandleActor> createActor(QString sourceKey,
                                                        quint64 sourceImageCacheKey,
                                                        std::shared_ptr<const QImage> sourceImage);
    void shutdown();

signals:
    void warning(const QString &message);
};
```

`photo_editor_init` 必须作为单独 GL task 提交：

```text
RuntimeExecutor task #1:
  photo_editor_init
```

不能在 init task 中顺便执行 `photo_editor_create`。

## 7. PhotoEditorHandleActor

`PhotoEditorHandleActor` 是本方案的业务核心。每个 actor 对应一张图和一个 `photo_editor` handle。

它不是线程，也不拥有 GL context。它只是一个有状态的 mailbox，将业务动作转成单步 GL task，提交给 `RuntimeExecutor`。

actor 的 public API 必须按线程安全入口设计。调用方可以来自 UI 线程、业务 worker 线程，或由 SDK callback 间接触发的线程。无论调用方来自哪个线程，actor 都不能在调用线程直接触碰 `photo_editor_*` 或 GL 资源，只能更新自身线程安全状态，并向 `RuntimeExecutor` 投递单步 GL task。

建议状态：

```text
Empty
Creating
Ready
Processing
Processed
Rendering
Rendered
Destroying
Destroyed
Failed
```

建议持有数据：

```cpp
class PhotoEditorHandleActor final : public QObject
{
    Q_OBJECT

private:
    RuntimeExecutor *m_executor = nullptr;
    void *m_handle = nullptr;
    QString m_sourceKey;
    quint64 m_sourceImageCacheKey = 0;
    std::shared_ptr<const QImage> m_sourceImage;
    ImageEffectParameters m_latestParameters;
    QSize m_outputSize;
    quint64 m_generation = 0;
    State m_state = State::Empty;
    bool m_destroyRequested = false;
    bool m_processAgainRequested = false;
    mutable QMutex m_mutex;
};
```

建议公开动作：

```cpp
void create();
void setOutputSize(QSize size);
void setOpcode(ImageEffectParameters parameters);
void process();
void render();
void destroy();
```

这些动作只负责编排和投递 task，不直接触碰 GL 资源。真正的 `photo_editor_*` 调用只能发生在 executor task 内。

其中 `setOutputSize()`、`setOpcode()`、`process()` 必须支持跨线程调用。`create()` 和 `destroy()` 也建议按线程安全实现，但业务入口上可以由 `PhotoEditorRuntimeService` 或 `PhotoEditorAppSession` 统一调用。

## 8. 线程安全调用模型

`PhotoEditorHandleActor` 的线程安全边界建议如下：

- 所有 public 方法都可以被任意线程调用。
- public 方法内部必须用 mutex 保护 actor 状态。
- public 方法只做状态更新、请求折叠、generation 读取和 task 投递。
- public 方法不得直接调用 `photo_editor_*`。
- GL task 执行时如需读取 actor 状态，必须先复制快照，再释放锁执行 `photo_editor_*`。
- GL task 完成后如需回写状态，必须再次加锁，并校验 generation。

推荐模式：

```cpp
void PhotoEditorHandleActor::setOpcode(ImageEffectParameters parameters)
{
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_state == State::Destroyed || m_state == State::Destroying) {
            return;
        }

        m_latestParameters = parameters;
        generation = m_generation;
    }

    postSetOpcodeTask(parameters, generation);
}
```

GL task 中必须再次校验：

```cpp
executor->post([actor = weakFromThis(), parameters, generation](IRuntime *runtime) {
    auto self = actor.lock();
    if (!self || !self->isCurrentGeneration(generation)) {
        return;
    }

    void *handle = self->handleSnapshot(generation);
    if (handle == nullptr) {
        return;
    }

    QString error;
    RuntimeScope scope(runtime, &error);
    if (!scope.ok()) {
        self->failIfCurrent(generation, error);
        return;
    }

    if (!photo_editor_set_opcode(handle, parameters, &error)) {
        self->failIfCurrent(generation, error);
    }
});
```

这样可以保证跨线程调用只影响 actor mailbox，不会破坏 GL 线程的单步执行模型。

## 9. 单步 GL Task 约束

这是本方案最重要的验证约束。

允许：

```cpp
executor->post([actor = weakFromThis(), image](IRuntime *runtime) {
    auto self = actor.lock();
    if (!self) {
        return;
    }

    QString error;
    RuntimeScope scope(runtime, &error);
    if (!scope.ok()) {
        self->fail(error);
        return;
    }

    void *handle = photo_editor_create(*image, &error);
    self->onCreateFinished(handle, error);
});
```

禁止：

```cpp
executor->post([=](IRuntime *runtime) {
    photo_editor_create(*image, &error);
    photo_editor_set_output_size(handle, size, &error);
    photo_editor_set_opcode(handle, parameters, &error);
    photo_editor_process(handle, context, callback, userData, &error);
});
```

也就是说，即使多个函数在业务上天然连续，也必须拆成多个 task：

```text
task #1: photo_editor_create
task #2: photo_editor_set_output_size
task #3: photo_editor_set_opcode
task #4: photo_editor_process
task #5: photo_editor_render
```

## 10. Actor 状态流转

基础流转：

```text
Empty
  -> create()
Creating
  -> onCreateFinished()
Ready
  -> setOutputSize()
Ready
  -> setOpcode()
Ready
  -> process()
Processing
  -> SDK callback isEnd=true
Processed
  -> render()
Rendering
  -> onRenderFinished()
Rendered
```

销毁流转：

```text
Ready / Processing / Processed / Rendering / Rendered / Failed
  -> destroy()
Destroying
  -> photo_editor_destroy task finished
Destroyed
```

失败流转：

```text
任意非 Destroyed 状态
  -> unrecoverable error
Failed
```

关键规则：

- `create()` 只能从 `Empty` 发起。
- `setOutputSize()` 和 `setOpcode()` 在 `Ready`、`Processed`、`Rendered` 中可以直接排队，且允许从任意线程调用。
- `process()` 只能在 handle 已创建后发起，且允许从任意线程调用。
- `Processing` 状态下再次 `process()` 不应重入 `photo_editor_process`，只能拒绝或设置 `m_processAgainRequested`。
- SDK callback 到达时不直接调用 `photo_editor_render`，只触发 actor 提交新的 render task。
- `destroy()` 必须提升 generation，屏蔽旧 callback 和旧 task。
- 状态读写必须受 actor mutex 保护。
- GL task 中的状态回写必须校验 generation，避免跨线程旧请求覆盖新状态。

## 11. Process Callback 安全

`photo_editor_process` 是特殊函数，因为它会启动 SDK 异步流程并通过 callback 通知完成。

actor 每次 process 必须记录当前 active process generation。不要把由 actor 内部容器管理的临时 ticket 裸指针传给 SDK callback；否则旧 timer callback 可能在 ticket 释放后继续解引用 `userData`。

```cpp
quint64 m_activeProcessGeneration = 0;
```

`photo_editor_process` 的 `userData` 可以传 actor 本身。callback 只做轻量转发，不直接触碰 GL：

```cpp
void onPhotoEditorProgress(int progress, bool isEnd, void *userData)
{
    auto *actor = static_cast<PhotoEditorHandleActor *>(userData);
    if (actor == nullptr) {
        return;
    }

    actor->handlePhotoEditorProgress(progress, isEnd);
}
```

`handlePhotoEditorProgress()` 必须在 mutex 内读取并检查当前 active generation：

```cpp
void PhotoEditorHandleActor::handlePhotoEditorProgress(int progress, bool isEnd)
{
    quint64 generation = 0;
    bool shouldComplete = false;
    {
        QMutexLocker locker(&m_mutex);
        generation = m_activeProcessGeneration;
        shouldComplete = isEnd
            && generation != 0
            && generation == m_generation
            && m_state == State::Processing
            && !m_destroyRequested;
    }

    if (shouldComplete) {
        onProcessCompleted(generation, progress);
    }
}
```

`onProcessCompleted()` 仍必须再次检查 generation：

```cpp
void PhotoEditorHandleActor::onProcessCompleted(quint64 generation)
{
    if (generation != m_generation || m_state != State::Processing) {
        return;
    }

    m_state = State::Processed;
    render();
}
```

`render()` 会提交新的单步 GL task。callback 不能直接执行 `photo_editor_render`。

由于 callback 可能与 UI 线程或业务线程并发触发 actor 方法，`onProcessCompleted()` 也必须按线程安全入口处理：先加锁校验 generation 与状态，再释放锁提交独立 render task。

## 12. 合并与丢弃策略

framework 不提供合并与丢弃。actor 可以根据业务需要实现局部策略。

建议策略：

- `setOutputSize()`：线程安全 latest-only。若已有待执行 size，可在 mutex 内替换为最新值。
- `setOpcode()`：线程安全 latest-only。快速拖动参数时只在 mutex 内保留最新参数。
- `process()`：线程安全请求入口。若当前 `Processing`，在 mutex 内设置 `m_processAgainRequested = true`，当前 process 完成并 render 后再决定是否重新 process。
- `render()`：只接受当前 generation 的结果。
- `destroy()`：最高优先级。destroy 后旧 callback、旧 render、旧 result 全部忽略。

注意：这些策略属于 actor，不属于 executor。

## 13. 多 Handle 交错执行

每个 actor 管理自己的 handle，但所有 actor 共享同一个 `RuntimeExecutor`。

因此业务上可以同时存在多个 actor，GL 执行仍然全局串行。

示例：

```text
GL queue:
  actor#1 photo_editor_create
  actor#1 photo_editor_set_opcode
  actor#2 photo_editor_create
  actor#1 photo_editor_process
  actor#2 photo_editor_set_output_size
  actor#1 photo_editor_render
  actor#2 photo_editor_process
```

这可以验证两点：

- execution 层不关心 handle 属于哪张图。
- 每个 task 都只是一段需要 GL context 的 `void()`。
- actor public API 可以被多线程并发调用，但最终进入同一个 executor 后仍按全局顺序执行。

## 14. Demo 改造要求

`PhotoEditorAppSession` 不再直接调用 `photo_editor_*`。

它的职责调整为：

- 加载图片目录。
- 为当前图片创建或选择 actor。
- 将 UI 参数变化转发给 actor。
- 接收 actor 的 render result。
- 调用 platform writer 发布纹理。
- 接收 actor warning 并转发给 UI。

原 `runGpuPreviewStep()` 应删除或完全退出主路径。

demo 必须能产生日志证明每个 GL task 只执行一个函数：

```text
[GL] actor#1 photo_editor_init
[GL] actor#1 photo_editor_create
[GL] actor#1 photo_editor_set_output_size
[GL] actor#1 photo_editor_set_opcode
[GL] actor#1 photo_editor_process
[SDK] actor#1 process completed
[GL] actor#1 photo_editor_render
[GL] actor#1 photo_editor_destroy
```

## 15. 验证场景

重构完成后，至少需要覆盖以下场景：

- 加载单张图片并完成 create、set size、set opcode、process、render。
- 快速切换多张图片，产生多个 handle actor。
- 快速拖动参数，产生多次 `setOpcode()`，验证 latest-only 策略。
- processing 中再次请求 process，验证不会重入 `photo_editor_process`。
- process 完成后 callback 触发独立 render task。
- actor destroy 后 SDK callback 到达，验证 generation 防护有效。
- render 前 output size 改变，验证 size task 与 render task 顺序正确。
- 多 actor 交错提交，验证 GL task 全局顺序执行。
- shutdown 中销毁所有 handle，验证无旧 callback 误触发 render。
- 从非 GL 线程并发调用 `setOutputSize()`、`setOpcode()`、`process()`，验证 actor 状态无数据竞争。
- UI 线程快速拖动参数，同时 worker 线程请求 process，验证 latest-only 与 process-again 策略稳定。

## 16. 建议落地步骤

第一步：新增 framework 执行器。

- 新增 `src/framework/execution/runtime_executor.h`
- 新增 `src/framework/execution/runtime_executor.cpp`
- 保留 `RuntimeHost`、`QtRuntimeHost`。
- 删除历史 `AsyncLane`、`SyncLane` 实现，photo editor 主路径只使用 `RuntimeExecutor` 和业务 actor。

第二步：新增 photo editor service 与 actor。

- 新增 `src/app/photo_editor_runtime_service.h/.cpp`
- 新增 `src/app/photo_editor_handle_actor.h/.cpp`
- 将 `photo_editor_init`、`photo_editor_create`、`photo_editor_destroy` 迁入 service/actor 的单步 task。

第三步：改造 app session。

- `PhotoEditorAppSession` 持有 `PhotoEditorRuntimeService`。
- `PhotoEditorAppSession` 不再持有裸 `m_gpuEditorHandle`。
- 删除或绕开 `runGpuPreviewStep()`。
- 保留 CPU preview 逻辑作为独立同步业务，不影响 GL actor 设计。

第四步：补充日志与验证入口。

- 每个 actor task 打印 actor id、function name、generation。
- demo UI 操作仍保持当前行为，但内部提交路径必须是单步 task。
- 增加跨线程调用测试入口，至少覆盖 `setOutputSize()`、`setOpcode()`、`process()`。

## 17. 审查重点

请重点审查以下问题：

- `RuntimeExecutor` 是否足够纯粹，只提供 GL context + `void()` 顺序执行。
- `PhotoEditorHandleActor` 是否是合适的 handle 生命周期边界。
- 每个 GL task “最多一个 `photo_editor_*` 调用”的约束是否足够明确。
- `process -> render` 是否已经从同一个执行函数拆成 callback 后的新 task。
- generation token 是否足以覆盖 destroy、快速切图、旧 callback、旧 render result。
- latest-only 策略放在 actor 内是否合理。
- 多 actor 共享一个 executor 是否能覆盖后续“每个 handle 一张图”的扩展需求。
- `PhotoEditorHandleActor` 的 public API 线程安全边界是否足够清晰。
- `setOutputSize()`、`setOpcode()`、`process()` 跨线程调用时，是否仍能保证 GL task 单步执行与状态一致性。
