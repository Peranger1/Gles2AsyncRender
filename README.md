# Gles2AsyncRender

这是一个基于 Qt 5.15.2 的异步渲染示例工程，当前重点验证的模型是：

- UI 线程使用 `QOpenGLWidget` 负责显示
- worker 线程持有独立的共享 OpenGL 上下文
- GPU 算法逻辑全部在 worker 上下文内执行
- worker 每帧产出共享纹理 id，UI 线程直接消费并显示

当前工程主要面向 Windows + ANGLE + OpenGL ES 2.0 场景，同时也整理了后续扩展到 macOS / Linux 时需要遵守的集成边界。

## 当前架构

程序启动时会启用：

- `Qt::AA_UseOpenGLES`
- `Qt::AA_ShareOpenGLContexts`

整个工程的职责划分如下：

- UI 线程拥有 `QOpenGLWidget`
- worker 线程创建一个共享的 `QOpenGLContext`
- worker 上下文绑定到 `QOffscreenSurface`
- GPU 算法在 worker 上下文内部完成初始化和逐帧处理
- 最终输出通过共享纹理传回 widget 显示

这套结构已经对齐未来真实 GPU 算法库的接入形态，而不是单纯的演示代码结构。

## Windows 重点说明

在 Qt 5.15 的 Windows 环境下，启用 `AA_UseOpenGLES` 通常意味着走 ANGLE，底层常见实现是 D3D11。

这个组合在多线程共享 GLES 上下文时，稳定性关键点不是表面上的 OpenGL API，而是底层 D3D11 immediate context 的线程保护是否正确启用。

当前工程在运行时会尝试：

- 查询 Qt 当前上下文对应的 ANGLE `EGLDisplay`
- 解析 `EGL_D3D11_DEVICE_ANGLE`
- 启用 `ID3D11Multithread::SetMultithreadProtected(TRUE)`

相关结果会在启动日志中输出。

当前已经验证过的稳定路径是：

- 必须使用 Qt 实际加载的那一份 `libEGL[d].dll`
- 不能把 Qt 创建出来的 `EGLDisplay` 交给另一份 EGL 模块实例去调用
- 启用 D3D11 multithread protection 后，当前样例可以稳定启动、渲染、resize 和退出

## 真实算法库接入位置

真实 GPU 算法库应当接入：

- `src/shared_texture_worker.cpp`

建议替换当前内部演示后端 `DemoGpuAlgorithmBackend`，保留外围线程与上下文管理结构不变，只替换算法执行部分：

1. 在 worker 上下文 current 的情况下做一次性全局初始化
2. 在同一个 worker 上下文里执行逐帧 GPU 处理
3. 返回最终输出纹理 id

不要把重计算挪回 UI 线程，也不要改成 CPU 回读再上传的路径。

## 构建方式

```powershell
mkdir build\msvc64-build
cd build\msvc64-build
cmd /c 'call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "D:\Qt\5.15.2\msvc2019_64\bin\qmake.exe" ..\..\Gles2AsyncRender.pro && nmake'
```

生成的可执行文件位于：

```text
build\msvc64-build\release\Gles2AsyncRender.exe
```

## 文档说明

故障历史、根因分析与修复过程见：

- `docs/FAILURE_ANALYSIS.md`

Windows / macOS / Linux 三平台集成边界与适配建议见：

- `docs/CROSS_PLATFORM_INTEGRATION.md`
