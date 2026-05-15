# Gles2AsyncRender Architecture

## 概述

当前仓库已经收敛到一套统一的异步渲染主链路：

- UI 线程持有 `QOpenGLWidget`
- worker 线程持有独立 ANGLE runtime
- GPU 结果通过 D3D11 shared texture 发布
- UI 只负责导入、复制和显示
- 请求调度采用 `SerialConflated` 策略
- 业务层通过 `AsyncTaskFacade` 接入统一执行框架

当前代码不再使用旧的 `LatestOnly*` 链路，也不再保留独立的旧 worker 试验入口。

## 当前分层

### `src/framework/core`

- 请求模型：`WorkEnvelope`、`RequestHints`、`JobResult`
- 调度器：`SerialConflatedWorkScheduler`
- 执行推进器：`SerialConflatedAsyncPipeline`
- 通用控制器：`AsyncJobController`
- 合并策略接口：`IRequestCoalescer`

这一层不关心 Qt widget、D3D11 细节或 photo editor 业务状态。

### `src/framework/backend`

- `platform_render_backend.h`
  - 平台 backend 总入口

- `win_angle_d3d11/angle_standalone_runtime.*`
  - Windows 独立 ANGLE runtime

- `win_angle_d3d11/d3d11_frame_publisher.*`
  - GPU 结果发布为 `FrameTicket`

- `win_angle_d3d11/d3d11_shared_slot_pool.h`
  - shared texture slot 生命周期与读写安全

### `src/framework/qt`

- `qopenglwidget_frame_view.*`
  - `QOpenGLWidget` 壳
  - 接收 `FrameTicket`
  - 通过 presenter 读取 pending 帧并复制到本地显示纹理

- `qt_angle_display_presenter.*`
  - ANGLE/EGL 导入与 blit 显示逻辑

### `src/app`

- `async_render_main_window.*`
  - app 主窗口

- `async_task_facade.*`
  - app-facing 通用任务 façade

- `photo_editor_async_render_facade.*`
  - 当前 photo editor 业务 façade
  - 管理图片目录、当前图片、当前参数
  - 分别驱动 GPU 预览和同步 CPU 预览

### `src/adapters/photo_editor`

- `photo_editor_work_processor.*`
  - GPU 异步处理 adapter

- `photo_editor_cpu_preview_processor.*`
  - 同步 CPU 预览 adapter

- `photo_editor_render_payload.h`
  - GPU 预览 payload

- `photo_editor_cpu_preview_payload.h`
  - CPU 预览 payload

## 当前请求模型

当前主实现使用两类 lane：

- `photo_editor.preview`
  - GPU 异步预览
  - `DeliverOnlyIfLatest`

- `photo_editor.cpu_preview`
  - 同步 CPU 检查预览
  - `AlwaysDeliver`

这说明当前框架已经不再默认“所有结果都必须是 GPU 帧”。

## 当前结果模型

框架当前支持：

- `GpuTextureResult`
  - 通过 publisher 转成 `FrameTicket`

- `CpuImageResult`
  - 直接作为 `JobResult` 交付

- `ICustomResult`
  - 作为扩展结果保留

## 当前线程边界

- UI 线程
  - `AsyncRenderMainWindow`
  - `QOpenGLWidgetFrameView`

- worker 线程
  - `PhotoEditorAsyncRenderFacade`
  - GPU `AsyncTaskFacade`
  - CPU preview `AsyncTaskFacade`

worker 线程负责业务请求组织和通用 pipeline 推进；UI 线程只接收最终结果并完成显示或检查。

## 当前仓库边界

当前仓库保留的稳定文档只有：

- [README.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/README.md)
- [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)
- [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)

其余早期计划、故障分析和试验性设计文档已经移除，避免继续与现行实现并存。
