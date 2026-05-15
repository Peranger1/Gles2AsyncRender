# Gles2AsyncRender

这是一个面向 `Qt 5.15.1 + QOpenGLWidget + ANGLE/GLES2 + D3D11 shared texture` 的异步图像处理工程。当前仓库已经从早期试验性链路收敛到一套统一主方案：

- UI 侧固定使用 `QOpenGLWidget`
- Windows 侧 worker 使用独立 ANGLE runtime
- worker 通过 D3D11 shared texture 发布 GPU 结果
- UI 通过 ANGLE/EGL 导入并复制到本地显示纹理
- 请求调度使用 `SerialConflated` 策略
- 框架同时支持 GPU 预览结果和同步 CPU 结果

当前主要面向 Windows。macOS 方向只保留在设计文档中，不在本仓库主实现内。

## 当前能力

- 导入图片目录并切换图片
- GPU 异步预览：亮度、对比度、缩放、平移、旋转、水平翻转、垂直翻转
- 同步 CPU 预览检查：对当前图片和当前参数生成一张 CPU 侧小预览图
- `SerialConflated` 请求合并：活动请求未完成时，只保留同 lane 最新待处理请求

## 当前主结构

- [src/app/async_render_main_window.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/async_render_main_window.cpp)
  - 主窗口、菜单、状态栏、参数面板
  - 管理显示 widget 和 worker 线程

- [src/app/photo_editor_async_render_facade.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/photo_editor_async_render_facade.cpp)
  - photo editor 业务 facade
  - 管理图片目录状态、参数状态、请求构造与结果解释

- [src/app/async_task_facade.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/app/async_task_facade.cpp)
  - app-facing 通用任务 façade
  - 统一装配 `AsyncJobController + SerialConflatedWorkScheduler + SerialConflatedAsyncPipeline`

- [src/framework/core/serial_conflated_async_pipeline.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/serial_conflated_async_pipeline.cpp)
  - 异步执行推进器
  - 同时支持 `GpuTextureResult`、`CpuImageResult`、`ICustomResult`

- [src/framework/core/serial_conflated_work_scheduler.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/core/serial_conflated_work_scheduler.cpp)
  - 单 lane 串行、pending 最新覆盖、publication 串行交付

- [src/framework/backend/win_angle_d3d11/](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/backend/win_angle_d3d11)
  - Windows/ANGLE runtime
  - D3D11 shared texture 发布桥
  - shared slot pool

- [src/framework/qt/qopenglwidget_frame_view.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/qt/qopenglwidget_frame_view.cpp)
  - `QOpenGLWidget` 显示壳
  - 导入 pending 帧并复制到 UI 本地显示纹理

- [src/adapters/photo_editor/](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/adapters/photo_editor)
  - photo editor GPU processor
  - photo editor 同步 CPU preview processor

## 运行方式

推荐使用脚本：

```powershell
powershell -ExecutionPolicy Bypass -File D:\Desktop\AI-Agent\Gles2AsyncRender\scripts\run-gles2asyncrender.ps1 -Mode app
```

## 构建方式

当前约定环境：

- Qt: `D:\CodePrograms\Qt\5.15.1\msvc2019_64`
- VS 环境脚本: `D:\CodePrograms\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`

构建命令：

```powershell
cmd /c "if not exist build\\qt5151-release mkdir build\\qt5151-release && call \"D:\CodePrograms\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" && cd /d build\\qt5151-release && \"D:\CodePrograms\Qt\5.15.1\msvc2019_64\bin\qmake.exe\" ..\\..\\Gles2AsyncRender.pro && nmake release"
```

## 当前文档

- [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)
  - 当前仓库代码结构与运行边界

- [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)
  - 跨平台异步渲染框架设计方案
