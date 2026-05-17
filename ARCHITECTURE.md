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
  - photo editor GPU session / CPU renderer / runtime host
  - photo editor 参数快照与 GLES2 backend

当前实现不再保留旧的 `RequestDispatcher`、`RequestChannel`、`IRequestHandler`、`photo_editor_demo_handlers` 主链路。

## 当前分层

### `src/framework/platform`

- 公共合同
  - `runtime.h`
  - `reader.h`
  - `writer.h`
  - `platform_backend.h`
  - `runtime_types.h`
  - `texture_types.h`

- Windows ANGLE D3D11 实现
  - `win_angle_platform_backend.*`
  - `win_angle_runtime.*`
  - `win_angle_texture_reader.*`
  - `win_angle_texture_writer.*`
  - `d3d11_shared_texture_slots.h`

- 辅助实现仍保留在 `src/framework/backend/win_angle_d3d11`
  - `qt_angle_egl_tools.*`
  - `gles2_proc_table.*`
  - `gles2_shader_utils.*`

平台层只负责 runtime、纹理导入、纹理发布，以及 backend 组装；不负责 Qt widget 或 demo 业务。

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

- `runtime_invoker.h`
  - 直接同步调用
  - runtime 同步调用

- `runtime_scope.*`
  - `IRuntime::enter()/leave()` 的 RAII 包装

- `async_lane.h`
  - 异步 lane
  - 当前主用策略是 `MergeWhileBusy`
  - active 结果必须交付
  - waiting 区域允许合并

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
  - 持有 `PhotoEditorRuntimeHost`
  - 持有 `RuntimeInvoker`
  - 持有 `AsyncLane<PhotoEditorGpuPreviewArgs, RawGpuTextureResult>`
  - 在 GPU 结果完成后决定是否调用 `IWriter`

- `async_render_main_window.*`
  - demo 主窗口

### `src/photo_editor`

- `photo_editor_render_args.h`
  - GPU/CPU 预览参数快照

- `photo_editor_runtime_host.*`
  - 项目内 `RuntimeHost` 实现

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
  - 实际执行者：`PhotoEditorGpuSession`

- CPU 同步预览
  - 参数类型：`PhotoEditorCpuPreviewArgs`
  - 执行方式：`RuntimeInvoker::callDirect()`
  - 实际执行者：`PhotoEditorCpuRenderer`

这意味着当前项目已经从“按 request type 注册 handler”切换成“按执行语义选择 invoker 或 lane”。

## 当前结果模型

执行层当前主用两类结果：

- `RawGpuTextureResult`
  - 只表达 worker runtime 中的原始 GPU 输出
  - 还不是 UI 可读纹理

- `CpuImageResult`
  - 直接作为 CPU 图像结果交付

GPU 结果在 app 的 `handleGpuPreviewCompleted()` 中调用 `IWriter` 转成 `TextureTicket`，然后再交给 UI。

## 当前线程与运行时边界

- UI 线程
  - `AsyncRenderMainWindow`
  - `TexturePresentWidget`

- app worker 线程
  - `PhotoEditorAppSession`
  - `PhotoEditorRuntimeHost`
  - `PhotoEditorGpuSession`

- runtime
  - `WinAngleRuntime`
  - 由 `PhotoEditorRuntimeHost` 管理进入/退出

`PhotoEditorGpuSession` 只在 runtime 上下文内触碰 `photo_editor_*` 和 GLES2 资源；`PhotoEditorAppSession` 只负责业务编排和结果发布。

## 当前错误模型

当前 app 层把错误分成两类：

- `initializationFailed`
  - 初始化级故障
  - UI 侧会弹错误框

- `requestWarning`
  - 普通请求失败、publish 失败或运行期警告
  - UI 侧只记录日志并短暂显示在状态栏

## 当前仓库边界

当前稳定文档包括：

- [README.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/README.md)
- [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)
- [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)

`docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md` 仍保留较多旧 `request_*` 结构说明，现在更适合作为历史重构记录，而不是当前实现说明。
