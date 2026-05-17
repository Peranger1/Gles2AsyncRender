# 框架重构头文件清单与类名分布方案

> 日期：2026-05-16
>
> 状态：Implemented with minor follow-ups
>
> 本文档最初用于指导下一轮代码重构。当前 `framework/platform + framework/execution + app + photo_editor` 主结构已经基本落地，因此本文档现在同时承担两种用途：
>
> - 作为当前头文件分层的实现说明
> - 作为迁移关系的历史记录
>
> 当前代码状态请同时参考：
>
> - [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)
> - [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)

## 0. 当前实现状态

截至当前代码版本，以下目标已经完成：

- `framework/platform` 公共合同与 Windows ANGLE D3D11 实现已落地
- `framework/execution` 已落地，并由 `RequestChannel` 根据 `RequestTypeDescriptor.queuePolicy` 自行创建 waiting policy
- `app/texture_present_widget.*`、`photo_editor_demo_*`、`photo_editor_app_session.*` 已落地
- 旧 `framework/core`、`framework/qt`、旧 photo editor processor/session 链路已删除
- `WinAngleRuntime` 已不再依赖旧 `angle_standalone_runtime.*`
- `WinAngleTextureWriter` 已直接持有发布逻辑，不再保留 `win_angle_texture_publish_bridge.*`

当前仍保留的少量后续工作主要是文档同步，而不是主代码结构迁移。

## 1. 本轮设计修正

相较于上一版总设计，当前结论有 4 个关键修正：

- “请求不可取消”是执行层的全局前提，不是某一种请求策略。
- 执行层只负责“请求提交、busy 时如何处理 waiting 请求、同步/异步完成、结果通知”，不负责纹理发布。
- `IWriter` 不由执行层持有。是否把 GPU 结果发布成 UI 可读纹理，由外部在结果完成后决定。
- 不再保留 `src/framework/qt`。`QOpenGLWidget` 及其 copy/blit 逻辑下放到 `src/app`。

因此，目标结构不再是旧文档中的 `framework/core + framework/platform + framework/qt + adapters/*`，而是以下 4 块主结构：

1. `framework/platform`
   - 运行时
   - 纹理读写
   - 平台 backend 总入口
2. `framework/execution`
   - 请求封装
   - busy 策略
   - 同步/异步执行
   - 结果完成通知
3. `app`
   - `QOpenGLWidget`
   - UI copy 到本地显示纹理
   - demo 业务 glue

当前实现已经把 photo editor 相关 session / renderer / backend 收敛到 `src/photo_editor`，不再单独保留 `adapters/photo_editor` 目录。

## 2. 目标目录结构

```text
src/
  framework/
    platform/
      runtime_types.h
      texture_types.h
      runtime.h
      reader.h
      writer.h
      platform_backend.h
      win_angle_d3d11/
        win_angle_platform_backend.h
        win_angle_runtime.h
        win_angle_texture_reader.h
        win_angle_texture_writer.h
        d3d11_shared_texture_slots.h
        qt_angle_egl_tools.h
        gles2_proc_table.h
        gles2_shader_utils.h
    execution/
      execution_types.h
      execution_context.h
      request_handler.h
      request_result_sink.h
      request_merge_strategy.h
      request_queue_policy.h
      merge_while_busy_policy.h
      serial_queue_policy.h
      request_channel.h
      request_dispatcher.h
  app/
    texture_present_widget.h
    photo_editor_demo_requests.h
    photo_editor_demo_handlers.h
    photo_editor_app_session.h
    async_render_main_window.h
  photo_editor/
    photo_editor_render_args.h
    photo_editor_runtime_host.h
    photo_editor_gpu_session.h
    photo_editor_cpu_renderer.h
    photo_editor_gles2_backend.h
```

说明：

- `framework/platform` 和 `framework/execution` 是真正可复用的框架层。
- `app` 是 Qt 与 demo 业务的落地点，不再假装自己是框架。
- `photo_editor` 是当前 photo editor 业务能力的集中落点。

## 3. `framework/platform` 头文件清单

### 3.1 `src/framework/platform/runtime_types.h`

放置：

- `enum class RuntimeKind`

建议枚举值：

- `None`
- `AngleGles2`
- `SharedDesktopOpenGl`

用途：

- 让执行层在请求描述里声明“是否需要 runtime、需要哪类 runtime”。
- 避免把平台实现类名直接泄漏到执行层。

### 3.2 `src/framework/platform/texture_types.h`

放置：

- `struct TextureTicket`
- `struct TextureLease`

建议定义：

```cpp
struct TextureTicket final
{
    int slotIndex = -1;
    quint64 generation = 0;
    quint64 frameIndex = 0;
    QSize size;
};

struct TextureLease final
{
    GLuint textureId = 0U;
    QSize size;
};
```

用途：

- `TextureTicket` 是“已发布到平台层、可被 UI 读取”的标准句柄。
- `TextureLease` 是 UI 在当前 GL context 下拿到的可读纹理借用。

### 3.3 `src/framework/platform/runtime.h`

放置：

- `class IRuntime`

职责：

- 初始化 worker runtime
- 进入当前线程上下文
- 离开当前线程上下文
- 释放 runtime
- 解析 GL proc

### 3.4 `src/framework/platform/reader.h`

放置：

- `class IReader`

职责：

- 绑定到当前 UI GL context
- 从 `TextureTicket` 获取可读纹理
- 释放读 lease
- context 销毁前做 reader 清理

建议接口：

```cpp
class IReader
{
public:
    virtual ~IReader() = default;
    virtual bool attachToCurrentContext(QString *error) = 0;
    virtual bool acquire(const TextureTicket &ticket, TextureLease *lease, QString *error) = 0;
    virtual void release(const TextureLease &lease) = 0;
    virtual void detach() = 0;
};
```

### 3.5 `src/framework/platform/writer.h`

放置：

- `class IWriter`

职责：

- 把 worker 侧 GPU 纹理发布成 UI 可读纹理
- 产出 `TextureTicket`
- 维护平台侧共享槽状态

建议接口：

```cpp
class IWriter
{
public:
    virtual ~IWriter() = default;
    virtual bool publishTexture(GLuint sourceTextureId,
                                const QSize &size,
                                TextureTicket *ticket,
                                QString *error) = 0;
    virtual void reset() = 0;
};
```

### 3.6 `src/framework/platform/platform_backend.h`

放置：

- `class IPlatformBackend`

职责：

- 创建 runtime
- 暴露 reader
- 暴露 writer

建议接口：

```cpp
class IPlatformBackend
{
public:
    virtual ~IPlatformBackend() = default;
    virtual std::unique_ptr<IRuntime> createRuntime() const = 0;
    virtual IReader *reader() const = 0;
    virtual IWriter *writer() const = 0;
};
```

这一层不再包含：

- `createPresenter()`
- `createPublisher()`
- 任何 Qt widget 相关接口

### 3.7 `src/framework/platform/win_angle_d3d11/win_angle_platform_backend.h`

放置：

- `class WinAnglePlatformBackend`

职责：

- Windows 平台总入口
- 组装 `WinAngleRuntime + WinAngleTextureReader + WinAngleTextureWriter`

### 3.8 `src/framework/platform/win_angle_d3d11/win_angle_runtime.h`

放置：

- `class WinAngleRuntime`

职责：

- 封装当前的独立 ANGLE worker runtime
- 当前实现中，旧 `angle_standalone_runtime.*` 已删除，能力已迁入 `WinAngleRuntime`

### 3.9 `src/framework/platform/win_angle_d3d11/win_angle_texture_reader.h`

放置：

- `class WinAngleTextureReader`

职责：

- 在 UI 当前 GL context 中导入 D3D11 shared texture
- 处理 EGL import
- 处理 keyed mutex acquire/release
- 对外只暴露 `IReader`

这部分对应旧 `qt_angle_display_presenter.cpp` 中“导入平台纹理”的职责，但不再承担 UI blit/present 职责。旧 `framework/qt` 已删除。

### 3.10 `src/framework/platform/win_angle_d3d11/win_angle_texture_writer.h`

放置：

- `class WinAngleTextureWriter`

职责：

- 把 worker runtime 中的输出纹理复制到共享槽
- 产出 `TextureTicket`

这部分承接了旧 `d3d11_frame_publisher.*` 的核心能力，但当前实现已经直接内聚在 `WinAngleTextureWriter` 中，不再保留额外桥接类。

### 3.11 `src/framework/platform/win_angle_d3d11/d3d11_shared_texture_slots.h`

放置：

- `class D3D11SharedTextureSlots`
- `struct D3D11SharedTextureSlot`

职责：

- 封装共享槽生命周期
- 提供 writer/readers 共享的内部状态

说明：

- 这是平台实现细节头文件，不属于上层公共合同。
- 旧 `shared_frame_slot_pool.h` 的职责已经迁入这里。

### 3.12 Windows 平台保留的辅助头文件

以下头文件保留，路径不必强制变化：

- `qt_angle_egl_tools.h`
- `gles2_proc_table.h`
- `gles2_shader_utils.h`

它们是 Windows 实现内部依赖，不是平台公共合同。

## 4. `framework/execution` 头文件清单

### 4.1 `src/framework/execution/execution_types.h`

放置：

- `using RequestId`
- `enum class DeviceKind`
- `enum class CompletionKind`
- `class IRequestPayload`
- `class ICustomResult`
- `struct RawGpuTextureResult`
- `struct CpuImageResult`
- `struct ExecutionRequest`
- `struct ExecutionResult`
- `struct RequestTypeDescriptor`

建议定义重点：

- `DeviceKind`：`Cpu` / `Gpu`
- `CompletionKind`：`Sync` / `Async`
- `RawGpuTextureResult`：这里只表达“原始 GPU 结果”，不能直接等价于 `TextureTicket`
- `ExecutionResult`：执行层完成后的标准结果
- `RequestTypeDescriptor`：由外部声明 request type 的设备类型、完成模式、runtime 需求

建议把 `RequestTypeDescriptor` 至少定义成：

```cpp
struct RequestTypeDescriptor final
{
    QString typeId;
    DeviceKind device = DeviceKind::Cpu;
    CompletionKind completion = CompletionKind::Sync;
    RuntimeKind runtimeKind = RuntimeKind::None;
};
```

### 4.2 `src/framework/execution/execution_context.h`

放置：

- `class IExecutionContext`

职责：

- 给 handler 提供可选 runtime 访问

建议接口：

```cpp
class IExecutionContext
{
public:
    virtual ~IExecutionContext() = default;
    virtual IRuntime *runtime() const = 0;
};
```

说明：

- `runtime()` 可以返回 `nullptr`
- 是否需要 runtime，由外部注册的 `RequestTypeDescriptor` 决定
- 当前 app 已在 `PhotoEditorAppSession::onResultReady()` 中使用回传的 `IExecutionContext`

### 4.3 `src/framework/execution/request_handler.h`

放置：

- `enum class StartDisposition`
- `class IRequestExecution`
- `class IRequestHandler`

职责：

- 统一同步与异步请求启动接口

建议语义：

- `CompletedInline`
- `StartedAsync`
- `Failed`

`IRequestExecution` 负责异步结果采集，`IRequestHandler` 负责启动请求。

### 4.4 `src/framework/execution/request_result_sink.h`

放置：

- `class IRequestResultSink`

职责：

- 接收“请求已完成”的结果通知
- 接收失败通知

注意：

- 不放通用 `progress`
- 不放纹理发布逻辑

建议接口：

```cpp
class IRequestResultSink
{
public:
    virtual ~IRequestResultSink() = default;
    virtual void onResultReady(const ExecutionResult &result,
                               IExecutionContext &context) = 0;
    virtual void onRequestFailed(RequestId requestId,
                                 const QString &error) = 0;
};
```

这里把 `IExecutionContext` 传回外部，是为了让外部在“结果完成当下”决定是否用 runtime + `IWriter` 做 GPU 结果再处理，但执行层本身不持有 `IWriter`。

### 4.5 `src/framework/execution/request_merge_strategy.h`

放置：

- `enum class MergeDisposition`
- `class IRequestMergeStrategy`
- `class ReplaceWithLatestMergeStrategy`

职责：

- 定义“同类型请求在 busy 时如何合并 waiting 请求”

说明：

- 只有 `MergeWhileBusyPolicy` 需要它
- `SerialQueuePolicy` 不需要 merge strategy

### 4.6 `src/framework/execution/request_queue_policy.h`

放置：

- `class IRequestQueuePolicy`
- `struct QueueSnapshot`

职责：

- 抽象“busy 时 waiting 区域如何管理”

说明：

- “不可取消 active 请求”不在这里建模，它是执行层全局前提
- queue policy 只关心 active 已存在时，新的 incoming request 怎么进入 waiting

### 4.7 `src/framework/execution/merge_while_busy_policy.h`

放置：

- `class MergeWhileBusyPolicy`

职责：

- active 运行时只维护一个 waiting
- 同类型请求交给 `IRequestMergeStrategy` 处理
- active 完成后再启动 waiting

这对应你要求中的：

- 请求不可取消
- 同类型请求在未完成时可以合并
- 当前请求完成后再继续新的请求

### 4.8 `src/framework/execution/serial_queue_policy.h`

放置：

- `class SerialQueuePolicy`

职责：

- active 运行时，后续请求按 FIFO 入队
- active 完成后严格按顺序继续执行

这对应你要求中的：

- 请求不可取消
- 请求不可合并
- 入队后按顺序执行

### 4.9 `src/framework/execution/request_channel.h`

放置：

- `class RequestChannel`

职责：

- 绑定一个 `RequestTypeDescriptor`
- 绑定一个 `IRequestHandler`
- 根据 `RequestTypeDescriptor.queuePolicy` 创建 `IRequestQueuePolicy`
- 管理一个 active 请求及其 waiting 区域
- 统一推进 sync/async 请求完成

这个类是新的执行层核心，不再保留旧的：

- `IWorkProcessor`
- `IWorkScheduler`
- `IAsyncPipeline`
- `AsyncJobController`

### 4.10 `src/framework/execution/request_dispatcher.h`

放置：

- `class RequestDispatcher`

职责：

- 管理多个 `RequestChannel`
- 按 `typeId` 分发请求
- 作为 app/demo 看到的执行层总入口

建议：

- 第一阶段不要设计成复杂多 lane 图
- 直接做“一个 request type 对应一个 channel”的注册模型

## 5. `app` 头文件清单

这一层不追求复用性，重点是把 Qt 和 demo glue 从框架层剥离出去。

### 5.1 `src/app/texture_present_widget.h`

放置：

- `class TexturePresentWidget`

职责：

- 继承 `QOpenGLWidget`
- 持有 `IReader *`
- 在 `initializeGL()` 中 `attachToCurrentContext()`
- 在 `paintGL()` 中：
  - `acquire(ticket, &lease)`
  - copy 到本地 display texture
  - `release(lease)`
  - 绘制本地 display texture

说明：

- 这里直接承担原 `framework/qt` 的 UI 工作
- 如果未来 macOS 的 UI blit 行为与 Windows 有明显分歧，再考虑是否需要 app 内部拆 presenter
- 现阶段不回升为 framework 层抽象

### 5.2 `src/app/photo_editor_demo_requests.h`

放置：

- `struct PhotoEditorGpuPreviewPayload`
- `struct PhotoEditorCpuPreviewPayload`

职责：

- 临时承接当前 demo 的业务 payload

说明：

- 因为当前 `photo_editor` 目录已经集中承载 backend / session / renderer
- 当前 photo editor 的请求形状先留在 app，不上升为框架公共模型

### 5.3 `src/app/photo_editor_demo_handlers.h`

放置：

- `class PhotoEditorGpuPreviewHandler`
- `class PhotoEditorCpuPreviewHandler`

职责：

- demo 侧请求处理器
- 调用 `src/photo_editor/photo_editor_gles2_backend.*`
- 向执行层返回 `RawGpuTextureResult` 或 `CpuImageResult`

说明：

- 这是 demo glue，不属于框架公共 adapter
- 目的是替代当前：
  - 旧 `photo_editor_work_processor.h`
  - 旧 `photo_editor_cpu_preview_processor.h`
  - 旧 `photo_editor_render_session.h`

### 5.4 `src/app/photo_editor_app_session.h`

放置：

- `class PhotoEditorAppSession`

职责：

- 管理图片目录、当前图像、当前参数
- 管理 `RequestDispatcher`
- 在 `onResultReady()` 中决定是否调用 `IWriter`
- 把 `TextureTicket` 或 CPU 结果转成 UI 可消费事件

说明：

- 这是旧 `photo_editor_async_render_facade.*` 的替代者
- 它是 app 层 session，不是框架 facade

### 5.5 `src/app/async_render_main_window.h`

放置：

- `class AsyncRenderMainWindow`

职责：

- 主窗口
- 菜单
- 参数控件
- 持有 `TexturePresentWidget`
- 持有 `PhotoEditorAppSession`

## 6. `photo_editor` 头文件清单

### 6.1 `src/photo_editor/photo_editor_gles2_backend.h`

保留：

- `photo_editor_init`
- `photo_editor_create`
- `photo_editor_destroy`
- `photo_editor_set_output_size`
- `photo_editor_set_opcode`
- `photo_editor_process`
- `photo_editor_render`

当前阶段不新增：

- `photo_editor_work_processor.h`
- `photo_editor_cpu_preview_processor.h`
- `photo_editor_render_session.h`
- `photo_editor_*_payload.h`

原因：

- 它们都在为旧执行框架服务
- 现在先把框架层边界重新理顺，再决定 photo editor 是否需要独立 adapter 目录形态

## 7. 头文件职责边界

必须遵守以下 include 方向：

- `framework/platform` 不能 include `QOpenGLWidget` 或 app 头文件
- `framework/execution` 不能 include Windows D3D11、EGL、Qt widget 头文件
- `app` 可以 include `framework/platform` 和 `framework/execution`
- `photo_editor` 可以 include `framework/platform` 和 `framework/execution`

还要遵守以下运行时边界：

- 执行层只交付 `ExecutionResult`
- 外部如果要把 `RawGpuTextureResult` 变成可显示纹理，必须在结果通知中显式调用 `IWriter`
- UI 侧只通过 `IReader` 获取可读纹理并 copy 到本地纹理

## 8. 现有文件到目标文件的迁移关系

### 8.1 保留并重命名/迁移

以下迁移当前已经完成：

- 旧 `work_runtime.h` 的职责已迁到 `src/framework/platform/runtime.h`
- 旧 `platform_render_backend.h` 的职责已迁到 `src/framework/platform/platform_backend.h`
- 旧 `angle_standalone_runtime.*` 的能力已迁入 `win_angle_runtime.*`
- 旧 `d3d11_frame_publisher.*` 的能力已迁入 `win_angle_texture_writer.*`
- 旧 `shared_frame_slot_pool.h` 的职责已迁入 `d3d11_shared_texture_slots.h`

### 8.2 下放到 app

以下迁移当前已经完成：

- 旧 `qopenglwidget_frame_view.*` 已由 `src/app/texture_present_widget.*` 替代
- 旧 `qt_angle_display_presenter.*` 已删除
- 其中平台纹理导入逻辑已迁入 `win_angle_texture_reader.*`
- 其中本地显示纹理 copy/blit 逻辑已迁入 `texture_present_widget.*`

### 8.3 删除并由新执行层替代

以下删除当前已经完成：

- [src/framework/core/async_pipeline.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/async_pipeline.h)
- [src/framework/core/async_job_controller.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/async_job_controller.h)
- [src/framework/core/work_processor.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/work_processor.h)
- [src/framework/core/work_scheduler.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/work_scheduler.h)
- [src/framework/core/work_observer.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/work_observer.h)
- [src/framework/core/serial_conflated_async_pipeline.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/serial_conflated_async_pipeline.h)
- [src/framework/core/serial_conflated_work_scheduler.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/serial_conflated_work_scheduler.h)
- [src/framework/core/request_coalescer.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/request_coalescer.h)
- [src/framework/core/replace_with_latest_coalescer.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/replace_with_latest_coalescer.h)

替代物：

- `request_handler.h`
- `request_result_sink.h`
- `request_merge_strategy.h`
- `request_queue_policy.h`
- `request_channel.h`
- `request_dispatcher.h`

### 8.4 从 adapter 退回 app glue

以下迁移当前已经完成：

- 旧 `photo_editor_async_render_facade.*` 已由 `photo_editor_app_session.*` 替代
- 旧 `photo_editor_work_processor.*` 已删除
- 旧 `photo_editor_cpu_preview_processor.*` 已删除
- 旧 `photo_editor_render_session.*` 已删除
- 旧 `photo_editor_render_payload.h` 与 `photo_editor_cpu_preview_payload.h` 已迁入 `photo_editor_demo_requests.h`

## 9. 第一阶段最小实现批次

本文原本建议的第一阶段批次当前已经基本全部完成。保留本节仅用于说明当前代码是按怎样的迁移顺序收敛过来的。

## 10. 最终结论

当前代码的头文件清单与职责边界应以本文和 [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md) 为准，核心原则只有 3 条：

- 平台层只提供 `IRuntime / IReader / IWriter / IPlatformBackend`
- 执行层只提供请求执行与结果完成通知，不持有 `IWriter`
- UI 侧直接在 app 中通过 `IReader` 获取纹理并 copy 到本地显示纹理

如果后续要开始实际改代码，建议第一批只先落这 3 个头文件族：

- `framework/platform/*`
- `framework/execution/*`
- `app/texture_present_widget.h`

这样可以先把新的边界定住，再迁移 photo editor demo。
