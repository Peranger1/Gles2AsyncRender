# Gles2AsyncRender

这是一个面向 `Qt 5.15.1 + QOpenGLWidget + ANGLE/GLES2 + D3D11 shared texture` 的异步图像处理工程。

当前主实现聚焦 Windows，已经验证以下主链路：

- UI 侧固定使用 `QOpenGLWidget`
- worker 侧使用独立 ANGLE runtime
- GPU 结果通过 D3D11 shared texture 发布
- UI 侧导入共享纹理并复制到本地显示纹理
- 执行层同时支持 GPU 异步结果和 CPU 同步结果

macOS 方向目前仍保留在设计文档中，不在本仓库主实现内。

## 当前能力

- 导入图片目录并切换图片
- GPU 异步预览
  - 亮度
  - 对比度
  - 缩放
  - 平移
  - 旋转
  - 水平翻转
  - 垂直翻转
- CPU 同步预览检查
  - 对当前图片和当前参数生成一张 CPU 小预览图
- 执行语义
  - GPU 预览使用 `MergeWhileBusy`
  - 已启动的 GPU 任务结果必须交付
  - waiting 区域允许折叠
  - CPU 预览直接同步执行，不进入 lane

## 当前架构

当前代码已经收敛到四块主结构：

- `framework/platform`
  - `IRuntime`
  - `IReader`
  - `IWriter`
  - `IPlatformBackend`
  - Windows ANGLE D3D11 平台实现

- `framework/execution`
  - `ExecutionOutcome`
  - `RuntimeHost`
  - `RuntimeInvoker`
  - `RuntimeScope`
  - `AsyncLane`
  - `SyncLane`
  - `QueuePolicyKind`
  - `DeliveryPolicyKind`

- `app`
  - `TexturePresentWidget`
  - `PhotoEditorAppSession`
  - `AsyncRenderMainWindow`

- `photo_editor`
  - `PhotoEditorRuntimeHost`
  - `PhotoEditorGpuSession`
  - `PhotoEditorCpuRenderer`
  - `photo_editor_render_args`
  - `photo_editor_gles2_backend`

当前实现不再保留旧的 `RequestDispatcher`、`RequestChannel`、`IRequestHandler`、`photo_editor_demo_handlers` 主链路。

## 关键文件

- [src/app/async_render_main_window.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/async_render_main_window.cpp)
  - 主窗口、菜单、参数面板、状态栏

- [src/app/texture_present_widget.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/texture_present_widget.cpp)
  - `QOpenGLWidget` 显示壳
  - 通过 `IReader` 获取 `TextureTicket`
  - 复制到本地显示纹理并绘制

- [src/app/photo_editor_app_session.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/photo_editor_app_session.cpp)
  - 管理图片目录、当前图片、当前参数
  - 持有 `PhotoEditorRuntimeHost`、`RuntimeInvoker`、`AsyncLane`
  - 在 GPU 结果完成后调用 `IWriter`

- [src/photo_editor/photo_editor_gpu_session.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor/photo_editor_gpu_session.cpp)
  - GPU runtime 亲和状态对象
  - 管理 `photo_editor_*` 生命周期
  - 发起异步 GPU 处理并收集 `RawGpuTextureResult`

- [src/photo_editor/photo_editor_cpu_renderer.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor/photo_editor_cpu_renderer.cpp)
  - CPU 同步预览生成

- [src/photo_editor/photo_editor_runtime_host.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor/photo_editor_runtime_host.cpp)
  - 项目内 `RuntimeHost` 实现

- [src/photo_editor/photo_editor_render_args.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor/photo_editor_render_args.h)
  - GPU/CPU 预览参数快照

- [src/framework/execution/async_lane.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/execution/async_lane.h)
  - 通用异步执行 lane
  - 提供 `MergeWhileBusy` 等 waiting 策略

- [src/framework/execution/runtime_invoker.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/execution/runtime_invoker.h)
  - 普通同步调用和 runtime 同步调用入口

- [src/framework/platform/win_angle_d3d11/win_angle_runtime.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/win_angle_d3d11/win_angle_runtime.cpp)
  - Windows worker ANGLE runtime

- [src/framework/platform/win_angle_d3d11/win_angle_texture_writer.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/win_angle_d3d11/win_angle_texture_writer.cpp)
  - GPU 结果发布为 `TextureTicket`

- [src/framework/platform/win_angle_d3d11/win_angle_texture_reader.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/win_angle_d3d11/win_angle_texture_reader.cpp)
  - UI 侧导入共享纹理并生成 `TextureLease`

- [src/photo_editor/photo_editor_gles2_backend.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor/photo_editor_gles2_backend.cpp)
  - 底层 GLES2 photo editor backend

## 构建环境

当前已验证环境：

- Qt: `D:\CodePrograms\Qt\5.15.1\msvc2019_64`
- VS 环境脚本: `D:\CodePrograms\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`
- Debug 构建目录: [build/qt5151-debug](/D:/Desktop/AI-Agent/Gles2AsyncRender/build/qt5151-debug)

## 构建方式

推荐直接使用脚本：

```powershell
powershell -ExecutionPolicy Bypass -File D:\Desktop\AI-Agent\Gles2AsyncRender\scripts\run-gles2asyncrender.ps1 -Configuration Debug -Reconfigure -Build -KillExisting
```

如果只想运行已构建程序，可以省略 `-Reconfigure -Build`。

## 运行方式

推荐使用脚本：

```powershell
powershell -ExecutionPolicy Bypass -File D:\Desktop\AI-Agent\Gles2AsyncRender\scripts\run-gles2asyncrender.ps1 -Mode app
```

也可以直接运行已构建出的可执行文件：

- [build/qt5151-debug/debug/Gles2AsyncRender.exe](/D:/Desktop/AI-Agent/Gles2AsyncRender/build/qt5151-debug/debug/Gles2AsyncRender.exe)

## 文档

- [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)
  - 当前代码结构、线程边界、结果模型

- [docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md)
  - 更早一轮的头文件收敛与重构讨论
  - 其中关于 `request_channel` / `request_dispatcher` / `photo_editor_demo_handlers` 的表述已过时

- [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)
  - 更早一轮的跨平台总体设计文档
  - 其中部分 `framework/core` / `framework/qt` / `FrameTicket` / `SerialConflatedLane` 表述属于历史设计，不是当前代码最终命名
