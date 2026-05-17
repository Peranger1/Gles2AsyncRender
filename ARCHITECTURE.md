# Gles2AsyncRender Architecture

## 概述

当前仓库已经收敛到四块主结构：

- `framework/platform`
  - worker runtime
  - shared texture reader / writer
  - 平台 backend 总入口
- `framework/execution`
  - runtime host / invoker
  - lane 调度
  - sync / async 执行结果
  - waiting 合并语义
- `app`
  - `QOpenGLWidget`
  - UI 显示与业务 glue
- `photo_editor`
  - photo editor GPU session / CPU renderer
  - photo editor 参数快照与 GLES2 backend

当前实现不再保留旧的 `RequestDispatcher`、`RequestChannel`、`IRequestHandler`、`photo_editor_demo_handlers` 主链路。

## 当前分层

### `src/framework/platform`

- 公共合同
  - `runtime.h`
  - `reader.h`
  - `writer.h`
  - `platform_backend.h`
  - `texture_types.h`
  - `presentation_events.h`

- Windows ANGLE D3D11 实现
  - `win_angle_platform_backend.*`
  - `win_angle_runtime.*`
  - `win_angle_texture_reader.*`
  - `win_angle_texture_writer.*`
  - `d3d11_shared_texture_slots.h`

- macOS Cocoa OpenGL + IOSurface 实现
  - `mac_cocoa_gl_platform_backend.*`
  - `mac_cocoa_gl_runtime.*`
  - `mac_cocoa_gl_texture_reader.*`
  - `mac_cocoa_gl_texture_writer.*`
  - `mac_iosurface_texture_slots.h`

- 辅助实现仍保留在 `src/framework/backend/win_angle_d3d11`
  - `qt_angle_egl_tools.*`
  - `gles2_proc_table.*`
  - `gles2_shader_utils.*`

平台层只负责 runtime、纹理导入、纹理发布，以及 backend 组装；不负责 Qt widget 或 demo 业务。

当前跨平台设计约束是：

- worker runtime 必须独立于 UI `QOpenGLContext`
- UI 与 worker 不共享 GL 对象
- 平台层负责把 worker 纹理发布到平台私有交换介质
- UI 侧始终通过 `TextureTicket` / `TextureLease` 消费结果

### `src/framework/execution`

- `execution_common.h`
  - `ExecutionOutcome`
  - `ExecutionError`
  - `TaskContext`
  - `LaneConfig`
  - `QueuePolicyKind`
  - `DeliveryPolicyKind`
  - `IWaitingMerger`

- `runtime_host.h`
  - runtime 线程调度合同

- `qt_runtime_host.*`
  - 基于 Qt 事件循环的 `RuntimeHost` 默认实现

- `runtime_invoker.h`
  - 直接同步调用
  - runtime 同步调用

- `runtime_scope.*`
  - `IRuntime::enter()/leave()` 的 RAII 包装

- `async_lane.h`
  - 异步 lane
  - 当前主用策略是 `MergeWhileBusy`
  - waiting 区域会先保留多个 checkpoint，再对队尾请求做 merge
  - 当前 GPU 预览实现使用 `DeliverEveryStartedResult`

- `sync_lane.h`
  - 同步 lane 基础接口
  - 当前项目中尚未成为主路径

执行层只负责“在哪执行、怎么排队、怎么交付结果”，不持有 `IWriter`，也不要求“每个业务函数一个 handler 类”。

### `src/app`

- `texture_present_widget.*`
  - `QOpenGLWidget`
  - 通过 `IReader` 获取 `TextureTicket`
  - copy 到本地显示纹理后绘制

- `photo_editor_app_session.*`
  - 管理目录、当前图片、当前参数
  - 持有 `QtRuntimeHost`
  - 持有 `RuntimeInvoker`
  - 持有 `AsyncLane<PhotoEditorGpuPreviewArgs, RawGpuTextureResult>`
  - 初始化时把 `RuntimeHost` / `IRuntime` attach 给 `IWriter`
  - 在 GPU 结果完成后调用 `IWriter::submitTexture()`

- `async_render_main_window.*`
  - demo 主窗口

### `src/photo_editor`

- `photo_editor_render_args.h`
  - GPU/CPU 预览参数快照

- `photo_editor_gpu_session.*`
  - GPU runtime 亲和状态对象
  - 管理 `photo_editor_init/create/set/process/render/destroy`

- `photo_editor_cpu_renderer.*`
  - CPU 同步预览生成

- `photo_editor_gles2_backend.*`
  - 底层 GLES2 photo editor backend

## 当前执行模型

当前主实现使用两类调用路径：

- GPU 异步预览
  - 参数类型：`PhotoEditorGpuPreviewArgs`
  - 执行器：`AsyncLane`
  - queue policy：`MergeWhileBusy`
  - delivery policy：`DeliverEveryStartedResult`
  - max waiting count：`3`
  - 实际执行者：`PhotoEditorGpuSession`

- CPU 同步预览
  - 参数类型：`PhotoEditorCpuPreviewArgs`
  - 执行方式：`RuntimeInvoker::callDirect()`
  - 实际执行者：`PhotoEditorCpuRenderer`

这意味着当前项目已经从“按 request type 注册 handler”切换成“按执行语义选择 invoker 或 lane”。

## 当前结果模型

当前项目的结果 payload 当前收敛在 `src/photo_editor/photo_editor_result_types.h`，而不是 `framework/execution`。

当前主用两类结果：

- `RawGpuTextureResult`
  - 只表达 worker runtime 中的原始 GPU 输出
  - 还不是 UI 可读纹理

- `CpuImageResult`
  - 直接作为 CPU 图像结果交付

GPU 结果在 app 的 `handleGpuPreviewCompleted()` 中调用 `IWriter` 转成 `TextureTicket`，然后再交给 UI。

当前 `IWriter` 发布合同是：

- `attach(RuntimeHost *, IRuntime *, IWriterEvents *)`
  - 显式注入 writer 需要依赖的调度器、runtime 和事件出口
- `submitTexture(GLuint sourceTextureId, QSize size, quint64 outputRevision, QString *error)`
  - 在调用方提供的 runtime 上下文里尝试立即发布
  - 若当前没有可立即发布的展示槽，writer 在平台层保留 latest pending frame
- `notifyPresentationCapacityAvailable()`
  - 由 reader 在释放展示槽后调用
  - writer 内部通过已 attach 的 `RuntimeHost` 回到 worker 线程继续 drain pending publish

当前 `TextureTicket` 除 `slotIndex / generation / frameIndex / size` 外，还携带：

- `outputRevision`
  - 由 `TexturePresentWidget` 在 resize 后递增
  - app 通过 `outputSizeChanged(size, revision)` 跟随 widget 的 revision
  - writer 在发布时把 revision 写入 `TextureTicket`
  - widget 只接收当前 revision 的 ticket，用于隔离 resize 前后的结果

平台结果发布当前也不是单 pending 语义，而是：

- `D3D11SharedTextureSlots` 维护多 `Ready` 槽队列
- writer 优先获取 `Free` 槽；池满时回收最老 `Ready` 槽
- 如果当前没有可立即进入展示链路的槽，writer 会把 latest pending frame 暂存在平台层
- reader 读取 `Ready` 槽后把它转成 `Reading`
- UI 复制完成后释放为 `Free`
- reader 在释放 `Reading` 槽后，直接通知 writer 当前出现新的 publish capacity
- writer 通过已 attach 的 `RuntimeHost` 回到 worker 线程自驱动 drain pending publish
- writer 通过 `IWriterEvents` 统一发出 `TextureTicket` 与 warning

## 当前线程与运行时边界

- UI 线程
  - `AsyncRenderMainWindow`
  - `TexturePresentWidget`

- app worker 线程
  - `PhotoEditorAppSession`
  - `QtRuntimeHost`
  - `PhotoEditorGpuSession`

- runtime
  - `WinAngleRuntime`
  - `MacCocoaGlRuntime`
  - 由 `QtRuntimeHost` 管理进入/退出

`PhotoEditorGpuSession` 只在 runtime 上下文内触碰 `photo_editor_*` 和 GLES2 资源；`PhotoEditorAppSession` 只负责业务编排和发起发布，不再负责平台补发链路。

当前状态补充：

- Windows 主链路已验证
- macOS `framework/platform` 已按独立 context + `IOSurface` slot 方向实现
- macOS 业务可运行性仍取决于 `photo_editor_gles2_backend` 从 Windows/ANGLE 假设中继续解耦

当前 resize 相关边界是：

- widget 是 `outputRevision` 的单一真相源
- resize 时 widget 清空本地 pending ticket 队列
- worker 收到新的 `outputRevision` 后会重新提交预览请求
- writer 若之后仍发布旧 revision 的 pending frame，UI 会基于 revision 过滤掉
- UI 若收到旧 revision 的 ticket，会直接丢弃

## 当前错误模型

当前 app 层把错误分成两类：

- `initializationFailed`
  - 初始化级故障
  - UI 侧会弹错误框

- `requestWarning`
  - 普通请求失败、publish 失败或运行期警告
  - UI 侧只记录日志并短暂显示在状态栏

## 当前仓库边界

当前正式文档只有：

- [README.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/README.md)
- [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)

其中：

- `README.md` 负责对外概览、构建运行方式、关键文件索引
- `ARCHITECTURE.md` 负责当前框架结构、执行模型、线程边界、结果模型

以下文档不再作为当前框架的正式描述来源，只保留为历史记录或方案讨论材料：

- [docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md)
- [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)

如果历史文档与当前代码、`README.md`、`ARCHITECTURE.md` 有冲突，一律以当前代码、`README.md`、`ARCHITECTURE.md` 为准。
