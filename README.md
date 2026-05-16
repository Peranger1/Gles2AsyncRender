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
- 请求调度
  - GPU 预览使用 `MergeWhileBusy`
  - CPU 预览使用 `SerialQueue`

## 当前架构

当前代码已经收敛到三层主结构：

- `framework/platform`
  - `IRuntime`
  - `IReader`
  - `IWriter`
  - `IPlatformBackend`
  - Windows ANGLE D3D11 平台实现

- `framework/execution`
  - `RequestTypeDescriptor`
  - `RequestChannel`
  - `RequestDispatcher`
  - `MergeWhileBusyPolicy`
  - `SerialQueuePolicy`

- `app`
  - `TexturePresentWidget`
  - `PhotoEditorDemo*`
  - `PhotoEditorAppSession`
  - `AsyncRenderMainWindow`

当前实现不再保留旧的 `framework/core`、`framework/qt`、`AsyncTaskFacade`、`photo_editor_*_processor/session` 主链路。

## 关键文件

- [src/app/async_render_main_window.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/async_render_main_window.cpp)
  - 主窗口、菜单、参数面板、状态栏

- [src/app/texture_present_widget.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/texture_present_widget.cpp)
  - `QOpenGLWidget` 显示壳
  - 通过 `IReader` 获取 `TextureTicket`
  - 复制到本地显示纹理并绘制

- [src/app/photo_editor_app_session.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/photo_editor_app_session.cpp)
  - 管理图片目录、当前图片、当前参数
  - 持有 `RequestDispatcher`
  - 在结果完成时调用 `IWriter`

- [src/app/photo_editor_demo_handlers.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/photo_editor_demo_handlers.cpp)
  - GPU 异步 preview handler
  - CPU 同步 preview handler

- [src/framework/execution/request_channel.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/execution/request_channel.cpp)
  - 单 request type 执行通道
  - 根据 `RequestTypeDescriptor.queuePolicy` 创建 waiting policy

- [src/framework/platform/win_angle_d3d11/win_angle_runtime.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/win_angle_d3d11/win_angle_runtime.cpp)
  - Windows worker ANGLE runtime

- [src/framework/platform/win_angle_d3d11/win_angle_texture_writer.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/win_angle_d3d11/win_angle_texture_writer.cpp)
  - GPU 结果发布为 `TextureTicket`

- [src/framework/platform/win_angle_d3d11/win_angle_texture_reader.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/platform/win_angle_d3d11/win_angle_texture_reader.cpp)
  - UI 侧导入共享纹理并生成 `TextureLease`

- [src/adapters/photo_editor/photo_editor_gles2_backend.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/adapters/photo_editor/photo_editor_gles2_backend.cpp)
  - 底层 GLES2 photo editor backend

## 构建环境

当前已验证环境：

- Qt: `D:\CodePrograms\Qt\5.15.1\msvc2019_64`
- VS 环境脚本: `D:\CodePrograms\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`
- 构建目录: [build/qt5151-release](/D:/Desktop/AI-Agent/Gles2AsyncRender/build/qt5151-release)

## 构建方式

在 `build\qt5151-release` 下执行：

```bat
call "D:\CodePrograms\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
"D:\CodePrograms\Qt\5.15.1\msvc2019_64\bin\qmake.exe" ..\..\Gles2AsyncRender.pro
nmake
```

当前这套 `qmake + nmake` 路径已经验证通过。

## 运行方式

推荐使用脚本：

```powershell
powershell -ExecutionPolicy Bypass -File D:\Desktop\AI-Agent\Gles2AsyncRender\scripts\run-gles2asyncrender.ps1 -Mode app
```

也可以直接运行已构建出的可执行文件：

- [build/qt5151-release/release/Gles2AsyncRender.exe](/D:/Desktop/AI-Agent/Gles2AsyncRender/build/qt5151-release/release/Gles2AsyncRender.exe)

## 文档

- [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)
  - 当前代码结构、线程边界、结果模型

- [docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md)
  - 当前头文件分层、迁移关系、重构收敛结果

- [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)
  - 更早一轮的跨平台总体设计文档
  - 其中部分 `framework/core` / `framework/qt` / `FrameTicket` / `SerialConflatedLane` 表述属于历史设计，不是当前代码最终命名
