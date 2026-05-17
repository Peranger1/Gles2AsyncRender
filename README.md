# Gles2AsyncRender

这是一个面向 `Qt 5.15.1 + QOpenGLWidget + 平台私有共享纹理交换` 的异步图像处理工程。

当前主实现聚焦 Windows，已经验证以下主链路：

- UI 侧固定使用 `QOpenGLWidget`
- worker 侧使用独立 runtime
- GPU 结果通过平台私有共享纹理发布
- UI 侧导入共享纹理并复制到本地显示纹理
- 执行层同时支持 GPU 异步结果和 CPU 同步结果

当前仓库已经补入 macOS 平台 backend 骨架，设计方向与 Windows 保持一致：

- worker 侧创建独立 `QOpenGLContext`
- UI / worker 不共享 GL 对象
- 平台层通过 `IOSurface` slot 交换纹理
- UI 侧仍只消费 `TextureTicket` / `TextureLease`

但当前 `photo_editor_gles2_backend` 仍主要绑定 Windows/ANGLE 假设，因此 macOS 端目前完成的是 `framework/platform` 交换链路，不是完整业务可运行态。

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
  - waiting 区域会先保留多个 checkpoint，再对队尾请求做 merge
  - 已启动的 GPU 任务结果当前会继续交付
  - CPU 预览直接同步执行，不进入 lane

## 当前架构

当前代码已经收敛到四块主结构：

- `framework/platform`
  - `IRuntime`
  - `IReader`
  - `IWriter`
  - `IPlatformBackend`
  - `platform_backend_factory`
  - Windows ANGLE D3D11 平台实现
  - macOS Cocoa OpenGL + `IOSurface` 平台实现

- `framework/execution`
  - `ExecutionOutcome`
  - `RuntimeHost`
  - `QtRuntimeHost`
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
  - 通过 `outputRevision` 过滤 resize 前后的旧票据
  - 复制到本地显示纹理并绘制

- [src/app/photo_editor_app_session.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/photo_editor_app_session.cpp)
  - 管理图片目录、当前图片、当前参数
  - 持有 `QtRuntimeHost`、`RuntimeInvoker`、`AsyncLane`
  - 在 GPU 结果完成后调用 `IWriter`
  - 初始化时把 `RuntimeHost` / `IRuntime` attach 给 `IWriter`
  - 按 widget 下发的 `outputRevision` 发布纹理结果

- [src/photo_editor/photo_editor_gpu_session.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor/photo_editor_gpu_session.cpp)
  - GPU runtime 亲和状态对象
  - 管理 `photo_editor_*` 生命周期
  - 发起异步 GPU 处理并收集 `RawGpuTextureResult`

- [src/photo_editor/photo_editor_cpu_renderer.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor/photo_editor_cpu_renderer.cpp)
  - CPU 同步预览生成

- [src/framework/execution/qt_runtime_host.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/execution/qt_runtime_host.cpp)
  - 基于 Qt 事件循环的 `RuntimeHost` 实现

- [src/photo_editor/photo_editor_render_args.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor/photo_editor_render_args.h)
  - GPU/CPU 预览参数快照

- [src/framework/execution/async_lane.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/execution/async_lane.h)
  - 通用异步执行 lane
  - 提供 `MergeWhileBusy` 等 waiting 策略
  - 支持 `DropStaleStartedResults`

- [src/framework/execution/runtime_invoker.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/execution/runtime_invoker.h)
  - 普通同步调用和 runtime 同步调用入口

- [src/framework/platform/win_angle_d3d11/win_angle_runtime.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/win_angle_d3d11/win_angle_runtime.cpp)
  - Windows worker ANGLE runtime

- [src/framework/platform/win_angle_d3d11/win_angle_texture_writer.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/win_angle_d3d11/win_angle_texture_writer.cpp)
  - GPU 结果发布为 `TextureTicket`
  - 共享纹理槽采用多 `Ready` 队列，而不是单 pending 槽
  - `IWriter` 显式依赖 `RuntimeHost` / `IRuntime` / `IWriterEvents`
  - 无空闲展示槽时由 writer 在平台层缓存 latest pending frame，并在槽释放后自驱动继续发布

- [src/framework/platform/win_angle_d3d11/win_angle_texture_reader.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/win_angle_d3d11/win_angle_texture_reader.cpp)
  - UI 侧导入共享纹理并生成 `TextureLease`

- [src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_platform_backend.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_platform_backend.cpp)
  - macOS 平台 backend 装配
  - worker 独立 `QOpenGLContext` + `IOSurface` slot 交换链路

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

## 当前调度与展示语义

- `AsyncLane` 的 GPU 预览主策略是：
  - `queue policy = MergeWhileBusy`
  - `delivery policy = DeliverEveryStartedResult`
  - `maxWaitingCount = 3`
- 这意味着：
  - active 请求不可取消
  - waiting 区域会优先保留最多 3 个中间 checkpoint
  - waiting 满后，新请求只和队尾 waiting tail 合并
  - started 请求一旦完成，当前实现仍会继续进入结果发布路径
- 平台纹理发布不是 latest-only：
  - `D3D11SharedTextureSlots` 维护多 `Ready` 槽队列
  - `IWriter::submitTexture()` 显式在调用方提供的 runtime 上下文里发布纹理
  - 无空闲展示槽时，writer 在平台层暂存 latest pending frame，而不是把 retry 责任抛回 app
  - `IReader` 在释放 reader lease 后直接通知 writer 当前出现新的 publish capacity
  - writer 通过已 attach 的 `RuntimeHost` 回到 worker 线程自驱动 drain pending publish
  - writer 通过 `IWriterEvents` 统一发出 `TextureTicket` 和 warning
  - UI 侧通过 `PlatformPresentationEvents::textureReady` 顺序消费可用 `TextureTicket`
  - `TextureTicket.outputRevision` 用于隔离 resize 前后的结果，避免旧尺寸帧继续显示

## 文档

- [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)
  - 当前框架的正式架构说明
  - 当前代码结构、线程边界、结果模型

- [docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md)
  - 历史重构记录，不是当前框架的正式文档
  - 其中关于 `request_channel` / `request_dispatcher` / `photo_editor_demo_handlers` 的表述已过时

- [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)
  - 历史方案文档，不是当前框架的正式文档
  - 其中部分 `framework/core` / `framework/qt` / `FrameTicket` / `SerialConflatedLane` 表述属于历史设计，不是当前代码最终命名

正式文档边界：

- `README.md` 与 `ARCHITECTURE.md` 是当前框架的正式文档来源
- `docs/*` 下的旧文档仅供查阅历史方案、迁移过程和设计背景
- 如有冲突，一律以当前代码、`README.md`、`ARCHITECTURE.md` 为准
