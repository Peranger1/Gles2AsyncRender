# Gles2AsyncRender Architecture

## 概述

当前仓库已经收敛到三层主结构：

- `framework/platform`
  - worker runtime
  - shared texture reader / writer
  - 平台 backend 总入口
- `framework/execution`
  - request type 描述
  - channel 调度
  - busy queue policy
  - sync / async 结果完成通知
- `app`
  - `QOpenGLWidget`
  - photo editor demo payload / handler / session
  - UI 显示与业务 glue

当前实现不再保留旧的 `framework/core`、`framework/qt`、`AsyncTaskFacade`、`photo_editor_*_processor/session` 主链路。

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

- `execution_types.h`
  - `RequestTypeDescriptor`
  - `ExecutionRequest`
  - `ExecutionResult`
  - `RawGpuTextureResult`
  - `CpuImageResult`

- `request_handler.h`
  - 请求启动接口
  - sync / async 执行抽象

- `request_result_sink.h`
  - 结果完成通知

- `request_merge_strategy.h`
  - busy 状态下的 waiting 合并规则

- `request_queue_policy.h`
  - waiting 区域管理接口

- `merge_while_busy_policy.h`
  - busy 时只保留一个 waiting，请求可合并

- `serial_queue_policy.h`
  - busy 时按 FIFO 入队

- `request_channel.*`
  - 绑定 request type、handler 和 queue policy
  - 根据 `RequestTypeDescriptor.queuePolicy` 自行创建 waiting policy

- `request_dispatcher.*`
  - app 看到的执行层总入口

执行层只负责请求推进与结果完成通知，不持有 `IWriter`。

### `src/app`

- `texture_present_widget.*`
  - `QOpenGLWidget`
  - 通过 `IReader` 获取 `TextureTicket`
  - copy 到本地显示纹理后绘制

- `photo_editor_demo_requests.h`
  - demo payload

- `photo_editor_demo_handlers.*`
  - GPU async preview handler
  - CPU sync preview handler

- `photo_editor_app_session.*`
  - 管理目录、当前图片、当前参数
  - 持有 `RequestDispatcher`
  - 在结果完成时决定是否调用 `IWriter`

- `async_render_main_window.*`
  - demo 主窗口

### `src/adapters/photo_editor`

- `photo_editor_gles2_backend.*`
  - 底层 GLES2 photo editor backend

当前 `adapters/photo_editor` 只保留底层处理能力，不再承载旧执行框架上的 processor / session 模板。

## 当前请求模型

当前主实现使用两个 request type：

- `photo_editor.preview.gpu.async`
  - GPU 异步预览
  - `DeviceKind::Gpu`
  - `CompletionKind::Async`
  - `RuntimeKind::AngleGles2`
  - `RequestQueuePolicyKind::MergeWhileBusy`

- `photo_editor.preview.cpu.sync`
  - 同步 CPU 检查预览
  - `DeviceKind::Cpu`
  - `CompletionKind::Sync`
  - `RuntimeKind::None`
  - `RequestQueuePolicyKind::SerialQueue`

## 当前结果模型

执行层当前支持三类结果：

- `RawGpuTextureResult`
  - 只表达 worker runtime 中的原始 GPU 输出
  - 还不是 UI 可读纹理

- `CpuImageResult`
  - 直接作为 CPU 图像结果交付

- `ICustomResult`
  - 作为扩展结果保留

GPU 结果在 app 的 `onResultReady()` 中调用 `IWriter` 转成 `TextureTicket`，然后再交给 UI。

## 当前线程与运行时边界

- UI 线程
  - `AsyncRenderMainWindow`
  - `TexturePresentWidget`

- worker runtime
  - `WinAngleRuntime`
  - `PhotoEditorGpuPreviewHandler`

- app session
  - `PhotoEditorAppSession`
  - 管理 `RequestDispatcher`
  - 处理执行完成后的结果发布

`IExecutionContext` 会在 `onResultReady()` 回传当前完成请求的 runtime；app 使用这个上下文判断 GPU 结果是否来自当前 session runtime，再决定是否发布。

## 当前仓库边界

当前稳定文档包括：

- [README.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/README.md)
- [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)
- [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)
- [docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md)

其中 `FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md` 负责描述当前重构收敛目标；本文件只描述当前代码已经落地的架构状态。
