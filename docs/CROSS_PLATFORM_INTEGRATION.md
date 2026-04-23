# Windows / macOS / Linux 三平台统一接入约束

本文定义当前这套多线程 GPU 渲染方案在 Windows、macOS、Linux 三个平台上的统一架构边界，以及各平台必须分开处理的后端差异。

目标场景是：

- UI 线程使用 `QOpenGLWidget` 显示结果
- worker 线程持有独立但共享的 `QOpenGLContext`
- 重计算依赖 OpenGL 上下文，并运行在 worker 线程
- 算法库最终返回可被 UI 直接消费的纹理 id

本文不讨论 CPU 图像回退，也不讨论 Vulkan、Metal、Direct3D 原生重写路线。

## 1. 统一架构

三平台应统一保持下面这个高层模型，不要按平台改变线程职责：

- UI 线程只负责 `QOpenGLWidget` 生命周期、窗口显示、resize 和最终呈现
- worker 线程只负责 GPU 算法库初始化、GPU 计算和输出纹理生成
- UI 和 worker 通过共享上下文共享纹理对象，不走 CPU 回读
- worker 上下文绑定到 `QOffscreenSurface`
- 算法库的一次性初始化和后续所有 GPU 调用，都必须在同一个 worker 上下文体系内完成

建议固定的数据流是：

1. UI 线程创建 `QOpenGLWidget`
2. widget 上下文创建完成后，派生 worker 的共享上下文
3. worker 线程在自己的 `QOffscreenSurface` 上 `makeCurrent()`
4. 算法库在 worker 上下文里做一次性初始化
5. 每帧由 worker 线程执行 GPU 处理，产出纹理 id
6. UI 线程接收最新纹理 id 后只做显示，不参与重计算

## 2. 不可破坏的统一约束

以下约束三平台都必须满足，否则这套方案不成立：

- `QOpenGLContext` 只能在所属线程上 `makeCurrent()`
- worker 渲染面必须使用 `QOffscreenSurface`
- 共享关系必须在上下文 `create()` 之前通过 `setShareContext()` 建立
- 算法库不能偷偷创建第二套不共享的 GL 运行时或主渲染设备
- 返回给 UI 的必须是共享组中有效的纹理对象，而不是库内部私有 FBO 的临时 attachment
- resize 时允许 worker 重建尺寸相关纹理和 FBO，但不能破坏共享组
- UI 线程不能接管算法库的 GPU 上下文，也不能在 widget 上下文里调用算法库的重计算接口

## 3. 为什么不能指望 Qt 自动统一多后端线程模型

Qt 提供的是跨平台 `QOpenGLContext` / `QSurface` 抽象，不会替应用统一不同图形后端的线程模型。

原因不在于“Qt 不允许多线程 OpenGL”，而在于：

- Windows 下可能是 ANGLE / D3D11，也可能是 Desktop OpenGL
- macOS 下是 `NSOpenGLContext` / `CGLContextObj`
- Linux 下可能是 EGL，也可能是 GLX
- “上下文共享”不等于“底层驱动对象天然支持并发访问”
- 算法库自己如何解析 GL / EGL 符号、是否自行加载运行时、是否内部再开线程，这些都超出 Qt 的控制范围

Qt 能提供的是：

- `AA_ShareOpenGLContexts`
- `QOpenGLContext::setShareContext()`
- `QOffscreenSurface`
- 平台原生句柄查询接口

真正的后端线程保护和原生适配，仍需要应用自行处理。

## 4. Windows 约束

### 4.1 当前项目的实际后端

在当前工程中：

- 启用了 `Qt::AA_UseOpenGLES`
- 在 Qt 5.15.2 / Windows 上，通常会落到 ANGLE
- ANGLE 的常见路径是 GLES2 on D3D11

所以表面上你在用 GLES2，底层真正的线程安全问题实际发生在 ANGLE + D3D11。

### 4.2 当前已验证可行的接入方式

已经验证通过的路径是：

- widget 线程和 worker 线程分别持有共享 `QOpenGLContext`
- worker 在 `QOffscreenSurface` 上执行 GPU 算法
- UI 只消费共享纹理
- 运行时通过 Qt 暴露的 native handle 找到 ANGLE 的 `EGLDisplay`
- 再从 Qt 已加载的同一份 `libEGL[d].dll` 模块解析 EGL 函数
- 最终查询 `EGL_D3D11_DEVICE_ANGLE` 并启用 `ID3D11Multithread`

这里最关键的约束不是“拿到 `EGLDisplay`”本身，而是下面三者必须来自同一套运行时实例：

- Qt 创建出来的 `EGLDisplay`
- Qt 实际加载的 `libEGL[d].dll`
- 算法库后续使用的 EGL / GLES 入口

### 4.3 Windows 下必须额外遵守的规则

- 不要在项目里再直接链接另一份 `libEGL` / `libGLESv2`
- 如果算法库会自己 `LoadLibrary` / `GetProcAddress`，必须保证它绑定的是 Qt 已加载的那份 ANGLE 模块
- 如果算法库要求 `EGLContext` / `EGLDisplay`，优先从 Qt 当前上下文的 native resource 提供，不要自行另起一套 EGL 初始化
- Windows 下“上下文共享可用”不等于“底层线程天然安全”，当前样例之所以稳定，是因为额外启用了 D3D11 multithread protection

### 4.4 D3D9 应该怎么理解

Qt 5.15.2 自带的 ANGLE 仍然可能保留 D3D9 分支，但它不应作为当前方案的主线。

原因很明确：

- D3D9 是旧路径，后续维护价值明显更低
- 当前稳定性修复依赖的是 `ID3D11Multithread`
- D3D9 的线程保护是在 device 创建时通过 flag 决定，不存在与当前 D3D11 方案等价、可在应用侧统一补上的运行时修补入口
- 而 Qt 当前又是自己创建 ANGLE display / context，应用并不能像现在修 D3D11 那样轻量接管 D3D9 初始化参数

因此对 D3D9 的结论是：

- 可以把它当成兼容性实验分支
- 不能把它当成当前生产化方案的统一基线

如果没有硬性历史包袱，不建议继续投资 D3D9。

## 5. macOS 约束

### 5.1 macOS 的本质差异

Qt 5.15 在 macOS 上走的是 Cocoa OpenGL 路径，底层是：

- `NSOpenGLContext`
- `CGLContextObj`

因此在 macOS 上：

- “worker 线程共享上下文 + `QOffscreenSurface` + 返回纹理 id” 这套高层架构仍然成立
- 但它不是 EGL / GLES 语义，而是 Desktop OpenGL / CGL 语义

### 5.2 macOS 应该如何实现

对当前项目，macOS 的正确实现方向是：

- 保留现有线程模型
- 不走 Windows 专用的 ANGLE / D3D11 补丁路径
- 不强制设置 `Qt::AA_UseOpenGLES`
- `QSurfaceFormat` 改为请求桌面 OpenGL，而不是 `OpenGLES`
- worker 仍然使用共享 `QOpenGLContext` + `QOffscreenSurface`
- 输出仍然是共享纹理 id，由 `QOpenGLWidget` 显示

更实际一点说，macOS 建议默认请求：

- `QSurfaceFormat::OpenGL`
- 版本 `2.1`
- `NoProfile`

这是因为当前样例 shader 是 ES2 风格，后续做跨平台兼容时，更容易先对齐到兼容性较高的桌面 OpenGL 路径。

### 5.3 macOS 对算法库的约束

如果算法库满足下面条件，则 macOS 可以沿用当前高层架构：

- 真正依赖的是“当前线程有可用 OpenGL 上下文”
- 不强依赖 `EGLDisplay` / `EGLContext`
- 能接受宿主提供的平台原生上下文，或只依赖当前 current context
- shader 和 GL 调用不被硬编码在 ANGLE / EGL 语义上

如果算法库满足下面任一条件，则 macOS 不能直接复用 Windows 的接法：

- API 明确要求 `EGLContext`
- 库内部必须自行加载 `libEGL` / `libGLESv2`
- shader、扩展、纹理对象语义完全绑定到 ANGLE / EGL

结论不是“macOS 不能做 worker 共享上下文”，而是“不能把 Windows 的 EGL/GLES 路径原样搬过去”。

## 6. Linux 约束

### 6.1 Linux 没有单一后端

Qt 5.15 在 Linux / X11 上常见有两类后端：

- GLX
- EGL

因此 Linux 不能被简单等同于“天然也是 EGL”。

### 6.2 Linux 的正确实现方式

Linux 上也应保持统一的高层结构：

- UI 线程显示
- worker 线程做 GPU 处理
- 通过共享上下文共享纹理

但 native 适配层必须按后端分支：

- 如果 Qt 当前是 EGL 集成，就提供 EGL 适配
- 如果 Qt 当前是 GLX 集成，就提供 GLX / Desktop OpenGL 适配

不要预设 Linux 一定有 EGL，也不要假设 Linux 上一定能照搬 Windows/ANGLE 的后端句柄模型。

### 6.3 Linux 对算法库的要求

在 Linux 上更理想的算法库形态是：

- 不强制依赖某一个窗口系统后端
- 不把 `EGLContext` 当作唯一合法输入
- 可以依赖当前线程的 current context 工作，或允许宿主注入 EGL / GLX 原生上下文

如果库只支持 EGL，那么：

- 在 Qt/Xcb EGL 集成下可以继续评估
- 在 GLX 集成下就必须额外做 backend adapter，或者直接判定该路径不支持

## 7. 三平台统一结论

真正可以统一的只有：

- 上层 worker / render contract
- 上下文所有权规则
- 共享纹理的数据流

真正不能统一的是：

- 原生上下文句柄类型
- GL / EGL 符号解析方式
- 后端线程保护实现
- 是否存在 EGL
- 是否需要平台专用修补逻辑

所以生产化设计不要走“单一 EGL/GLES 代码路径覆盖三平台”的思路，而应该是：

- 一套统一的上层线程与渲染契约
- 每个平台一个 backend adapter

## 8. 推荐的算法库接入分层

建议把算法库接入拆成三层。

### 8.1 平台无关层

只定义统一接口，例如：

- `initialize(sharedContextInfo)`
- `process(inputTextures...) -> outputTextureId`
- `resize(width, height)`
- `shutdown()`

这一层不直接暴露 `EGLDisplay`、`NSOpenGLContext`、`GLXContext` 之类的原生类型。

### 8.2 平台适配层

按平台提供原生上下文信息：

- Windows / ANGLE adapter
- macOS / CGL adapter
- Linux / EGL adapter
- Linux / GLX adapter

这一层负责：

- 从 Qt 查询 native resource
- 校验当前上下文和共享组
- 处理平台特有的线程保护
- 把算法库需要的原生信息封装后交给库

### 8.3 算法库桥接层

这一层只负责调用算法库 API，不负责 Qt 生命周期管理，也不自行创建主渲染上下文。

## 9. 接入判定矩阵

### 9.1 可视为跨平台友好的算法库

满足越多越好：

- 不自行初始化第二套窗口系统或第二套 GL runtime
- 不硬依赖 `EGLDisplay`
- 支持共享上下文中的纹理输入输出
- 要么只依赖当前线程的 current context，要么允许宿主传入平台原生 context
- 不在库内部把同一个 context 跨线程使用

### 9.2 需要平台特化适配的算法库

存在任一项就需要 backend adapter：

- Windows 只支持 EGL / ANGLE
- macOS 只支持 `NSOpenGLContext` / `CGLContextObj`
- Linux 只支持 EGL 不支持 GLX
- 会自行加载 `libEGL` / `libGLESv2`
- 自己管理额外 worker thread 和额外 context

### 9.3 不适合当前架构的算法库

存在任一项就应重新评估整体方案：

- 强制要求独占主上下文
- 要求所有 GPU 调用都发生在 UI 线程
- 输出不是共享纹理，而是私有 swapchain/backbuffer
- 必须自行创建窗口和显示面

## 10. 当前项目的实际落地建议

对当前仓库，更实际的三平台策略应当是：

- Windows
  - 保持当前 `AA_UseOpenGLES + AA_ShareOpenGLContexts`
  - 保持从 Qt 已加载 ANGLE 模块解析 EGL 函数
  - 保持 D3D11 multithread protection 补丁

- macOS
  - 不再强制使用 GLES / EGL 语义
  - 保持共享上下文 + worker 架构
  - 改为基于 `NSOpenGLContext` / `CGLContextObj` 的 Desktop OpenGL 适配

- Linux
  - 启动时先识别 Qt 当前是 EGL 还是 GLX
  - 再进入对应 adapter
  - 不要把 Linux 直接等同于 EGL

## 11. 一句话结论

三平台真正能统一的是“Qt 共享上下文 + worker GPU 处理 + 共享纹理显示”这套架构；三平台不能统一的是底层 native backend。本项目正确的生产化方向不是强行用一套 EGL/GLES2 路径覆盖所有平台，而是保留统一上层 contract，再分别适配 Windows/ANGLE、macOS/CGL、Linux/EGL 或 GLX。
