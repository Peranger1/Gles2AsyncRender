# 故障分析

本文记录了在 Windows 环境下验证目标架构时出现过的问题、排查过程以及最终确认的稳定路径。验证条件如下：

- Qt `5.15.2`
- `Qt::AA_UseOpenGLES`
- `Qt::AA_ShareOpenGLContexts`
- `QOpenGLWidget`
- ANGLE / D3D11 后端

目标架构始终是：

- UI 线程拥有 `QOpenGLWidget`
- worker 线程在 `QOffscreenSurface` 上持有共享的 GLES2 上下文
- GPU 算法库完全运行在 worker 上下文内
- 算法库输出纹理 id
- widget 直接消费并显示这张共享纹理

这里的核心要求不是 CPU 图像处理，而是依赖 OpenGL 上下文的真实 GPU 计算链路。

## 当前状态

截至大约 `2026-04-24 01:16` 的验证结果，当前样例已经可以：

- 正常启动
- 正常渲染
- resize 后不再黑屏
- 正常退出

日志同时确认：

- widget 上下文和 worker 上下文解析到同一个 ANGLE `EGLDisplay`
- `ID3D11Multithread` 保护已经成功启用
- 之前出现的 `D3D11 CORRUPTION` 崩溃在本次验证运行中已消失

因此，当前项目已经不再处于“原因不明的 ANGLE 不稳定”阶段，主要阻塞项已经被定位并验证了可行修复路径。

## 当前结论

最终确认的结论是：

- 当前目标架构本身可行
- 共享纹理设计不是崩溃根因
- 真正决定性的根因是 ANGLE 补丁路径中曾经把 Qt 持有的 EGL 句柄和另一份模块实例中的 EGL 入口混用了
- 当 EGL 函数改为从 Qt 实际加载的 ANGLE 模块中解析，并启用 `ID3D11Multithread` 之后，样例在验证运行中变得稳定

## 故障时间线

下面的故障记录来自同一轮调查窗口，大约覆盖 `2026-04-23 23:34` 到 `2026-04-24 00:33`。

### 1. 启动后立即崩溃

观察到的日志：

- widget 上下文创建成功
- worker 共享上下文创建成功
- 随后出现：
  - `D3D11 CORRUPTION: ID3D11DeviceContext::ClearRenderTargetView`
  - `Two threads were found to be executing functions associated with the same Device[Context] at the same time`

影响：

- 进程在启动后很快异常终止

根因分析：

- 虽然 Qt 暴露了多个共享 `QOpenGLContext`，但 ANGLE 最终仍可能把 GLES 工作映射到同一个底层 D3D11 immediate context
- UI 线程的绘制/合成和 worker 线程的 GLES 执行在同一个底层 device context 上发生了重叠
- 因此崩溃发生在应用层 OpenGL 抽象之下

采取的修改：

- 通过 `gles_thread_guard` 引入全局 GLES 互斥
- 在 widget 和 worker 中对显式 GL 区段做串行化

结果：

- 最直接的重叠有所减少
- 但没有彻底消除后端损坏问题

### 2. resize 不再直接崩，但画面变黑

观察到的日志：

- resize 本身不再崩溃
- 但窗口 resize 后画面变黑
- worker 报错：
  - `Shared texture worker error: "Failed to allocate shared textures."`

根因分析：

- 这部分问题不完全是后端线程冲突，还有应用层资源管理问题
- 纹理选择和分配错误上报不够严格
- 旧的 GL 错误状态可能让纹理分配阶段被误判为失败，而真正的问题发生在更早位置

采取的修改：

- 收紧 worker 后端中的纹理分配逻辑
- 把纹理选择放到真正分配之后
- 在纹理创建附近更严格地清理和复查 GL 错误点

结果：

- 黑屏现象被收敛到资源分配和交接路径
- 但 ANGLE 的主线程安全问题仍然存在

### 3. 缩短上下文生命周期后仍然崩溃

后续运行中观察到的日志：

- `D3D11 CORRUPTION: ID3D11DeviceContext::GetData`
- `D3D11 WARNING: ID3D11DeviceContext::End: End is being invoked on a Query, where the previous results have not been obtained with GetData`

根因分析：

- 单纯在应用层严格控制 `makeCurrent()` / `doneCurrent()` 生命周期还不够
- 即使缩短了 worker 上下文 current 的持续时间，ANGLE 仍可能在内部执行跨线程重叠的 D3D11 query 与同步操作
- 说明冲突并不只来自应用代码自己写下的那些显式 GL 调用

采取的修改：

- 让 worker 上下文只在短时处理窗口内保持 current
- 在 widget 合成和 resize 路径周围增加更宽范围的锁

结果：

- 不稳定仍然存在
- 如果 ANGLE 内部依然在多个线程上使用同一个 D3D11 immediate context，仅靠应用层加锁不足以完全解决

### 4. 临时评估过 CPU 回退方案，随后放弃

考虑过这条路的原因：

- 最保守的临时绕法，是把重计算搬离 GPU，只让 UI 线程负责上传纹理

为什么放弃：

- 未来真实算法库依赖 GPU / OpenGL 上下文
- 它会在给定上下文上做一次性全局初始化
- 它内部会执行多步 GPU 操作
- 最终直接返回纹理 id

结论：

- CPU 侧 `QImage` 回退不满足真实产品需求
- 架构必须维持 worker 上下文执行 GPU 任务的模型

### 5. 尝试在运行时修补 ANGLE 的 D3D11 多线程保护

推理依据：

- 如果能拿到 ANGLE 底层的 D3D11 device，那么启用 `ID3D11Multithread::SetMultithreadProtected(TRUE)` 就可能让这套架构在 ANGLE 上可行

增加的代码：

- `src/angle_threading.cpp`
- 运行时探测 EGL / ANGLE / D3D11 原生句柄
- 尝试访问：
  - `EGLDisplay`
  - `EGL_DEVICE_EXT`
  - `EGL_D3D11_DEVICE_ANGLE`
  - `ID3D11Multithread`

这一阶段遇到的失败包括：

1. 第一条探测路径：
   - `ANGLE threading patch failed: eglGetCurrentDisplay returned EGL_NO_DISPLAY.`

2. 后续探测路径：
   - Qt 原生句柄路径拿到了一个 `EGLDisplay` 候选
   - 但：
     - `ANGLE threading patch failed: eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed with EGL error 0x3008.`

3. 再后续的路径：
   - `nativeResourceForContext("egldisplay", context)` 返回了非空指针
   - 但用下面的方式验证时失败：
     - `eglQueryString(display, EGL_VERSION)`
     - `eglQueryString(display, EGL_VENDOR)`
   - EGL 错误为 `0x3008`
   - 同时：
     - `nativeResourceForIntegration("egldisplay")` 返回空
     - `nativeResourceForWindow("egldisplay", ...)` 被判定为非法 key
     - `eglGetCurrentDisplay()` 依然返回 `EGL_NO_DISPLAY`

4. 在这些失败之后，运行时仍然会出现：
   - `D3D11 CORRUPTION: ID3D11DeviceContext::GetData`
   - 或：
   - `D3D11 CORRUPTION: ID3D11DeviceContext::ClearRenderTargetView`

当时的解释是：

- 现有探测尚未真正打通到底层 D3D11 device
- Qt 返回的 native-resource 句柄看起来像某种包装层或间接句柄，而不是能直接喂给当前 EGL API 的 `EGLDisplay`
- window 和 integration 路径在当前环境下也没有暴露出可用的 EGL display
- 所以 `ID3D11Multithread` 实际上还没有启用成功
- 应用仍然运行在没有底层线程保护的 ANGLE/D3D11 模型上

### 6. 与 Qt 已加载的 EGL 模块对齐后，验证通过

观察到的日志：

- 启动成功
- resize 成功
- 进程退出成功
- widget 和 worker 都报告：
  - `ANGLE threading patch applied: ID3D11Multithread protection is enabled.`
- 两个上下文都报告了相同的 `EGLDisplay`
- `eglQueryString` 成功返回：
  - vendor: `Google Inc.`
  - version: `1.4 (ANGLE 2.1.0.57ea533f79a7)`

真正发生的变化：

- 探测路径不再依赖进程导入表里的 `egl*` 符号
- 而是改为从 Qt 已经加载的那一份 ANGLE `libEGL[d].dll` 模块中解析 EGL 函数
- 同时移除了项目对 `libEGL` 的直接链接依赖

最终确认的根因：

- Qt 的 `nativeResourceForContext("egldisplay")` 本身没有错
- 早先失败的根因是通过错误的模块实例在调用 EGL API
- 在 debug 配置下，Qt 实际加载的是 `libEGLd.dll` / `libGLESv2d.dll`
- 样例项目如果直接链接另一份 `libEGL`，就很容易把：
  - Qt 当前 ANGLE 运行时创建出来的 `EGLDisplay`
  - 和另一条导入路径上的 EGL 入口函数
  - 混在一起使用
- 一旦强制让 EGL 调用走 Qt 已加载的同一份模块，`EGLDisplay` 验证成功、device 查询成功、D3D11 multithread 补丁也真正生效

结果：

- 之前的 `D3D11 CORRUPTION` 崩溃在验证运行中消失
- resize 不再黑屏
- worker 线程共享上下文的总体设计得以保留

## 为什么当前互斥锁仍然不是万能解

当前互斥锁只能保护应用显式执行的 GL 代码区段，它无法完全控制：

- `QOpenGLWidget` 的内部合成行为
- ANGLE 自己的 D3D11 query
- Qt 或 ANGLE 内部触发的后端同步逻辑

这也是为什么即使应用层显式 GL 区段已经串行化，仍可能观察到 `GetData` 或 `ClearRenderTargetView` 这类后端损坏。

## 当前代码结构状态

当前代码被有意保留为未来真实 GPU 算法库接入所需的结构：

- `src/async_gles_widget.cpp`
  - 持有显示侧 `QOpenGLWidget`
  - 只消费并显示最新的共享纹理 id

- `src/shared_texture_worker.cpp`
  - 持有 worker 线程
  - 创建共享 GLES2 上下文
  - 在 worker 上下文内运行演示 GPU 后端
  - 把纹理 id 回传给 widget

- `src/angle_threading.cpp`
  - 探测 Qt 暴露的 EGL native handle
  - 尝试定位 ANGLE 背后的 D3D11 device
  - 尝试启用 `ID3D11Multithread`

这套形态在继续调试时应尽量保持不变，因为它已经对齐未来真实库的承载模型。

## 根因归类总结

当前问题实际上分成两类，不应混为一谈：

1. 应用层资源与生命周期问题
   - 例如 resize 黑屏、纹理分配路径问题
   - 这些问题可以在样例代码内部修复

2. 后端结构性线程冲突
   - 例如 `ClearRenderTargetView` / `GetData` 上的 `D3D11 CORRUPTION`
   - 这是 Qt 5.15.2 + ANGLE + `QOpenGLWidget` + worker 线程 GLES 的特定组合问题

在当前样例里，第 2 类问题已经不再是主要未解阻塞项，因为关键根因已经找到并处理。

## 后续修改策略

### 策略 A：保持当前已验证的 EGL 模块对齐规则

优先级最高的约束：

- 保留 `src/angle_threading.cpp` 中引入的运行时规则
- 持续从 Qt 实际加载的 ANGLE 模块解析 EGL 入口
- 不要重新引入会把应用绑定到另一份 EGL 模块实例的直接项目依赖

真实 GPU 算法库接入时必须重点确认：

- 它不会再额外加载另一份 ANGLE
- 它不会静态绑定到冲突的 EGL / GLES 运行时
- 它不会从 Qt 当前未使用的模块实例解析 EGL 入口
- 如果它需要自己解析 GL / EGL 符号，解析目标必须是当前持有上下文的那一套运行时

### 策略 B：保持 worker GPU 架构，不再回退 CPU

必须坚持：

- worker 持有 GPU 上下文
- worker 输出纹理 id
- UI 线程只做显示

原因很简单：

- 这是唯一符合未来真实 GPU 算法库契约的模型

### 策略 C：明确剩余的决策边界

当前样例已经证明以下几点成立：

- 可用的 `EGLDisplay` 确实存在
- `EGL_DEVICE_EXT` 可以查询
- `EGL_D3D11_DEVICE_ANGLE` 可以正确解析
- `ID3D11Multithread` 保护可以成功启用

所以，对当前目标集成方式而言，基于 ANGLE 的 worker 线程 GLES 路径是值得继续推进的。

但如果后续诊断证明下面任一情况出现：

- 真实算法库会加载或要求另一套冲突的 EGL / GLES 运行时
- 只有在接入真实算法库后，D3D11 损坏问题才重新出现
- 算法库内部还有超出当前锁假设的线程模型

那么应考虑的替代路线是：

1. 仍然保留 worker 线程 GPU 库设计，但 Windows 下改走 Desktop OpenGL，而不是 `AA_UseOpenGLES`
2. 继续保留 `AA_UseOpenGLES`，但重设算法库边界，不再让真实 GPU 工作发生在 worker 持有的 GL 上下文中

## 已记录下来的关键现象

本轮排查已经明确记录过以下关键现象：

- 启动时出现 `D3D11 CORRUPTION: ClearRenderTargetView`
- 后续出现 `D3D11 CORRUPTION: GetData`
- resize 后黑屏并伴随共享纹理分配失败
- `eglGetCurrentDisplay` 返回 `EGL_NO_DISPLAY`
- `eglQueryDisplayAttribEXT(EGL_DEVICE_EXT)` 返回 EGL 错误 `0x3008`
- `nativeResourceForContext("egldisplay")` 虽返回非空指针，但无法被 `eglQueryString` 接受
- `nativeResourceForIntegration("egldisplay")` 返回空
- `nativeResourceForWindow("egldisplay")` 被 `QWindowsNativeInterface` 拒绝
- 在改为从 Qt 已加载的 `libEGLd.dll` 解析 EGL 函数并启用 `ID3D11Multithread` 后，最终验证运行成功

这些现象强烈说明：主要矛盾并不是样例里的 shader、FBO 或纹理显示代码，而是这一特定 Qt/Windows/OpenGLES 组合下 ANGLE 后端的线程行为。

## 对最新成功日志的解释

最新成功运行使问题解释发生了变化：

- Qt 返回的 `egldisplay` 候选实际上是有效的
- widget 和 worker 最终解析到了同一个 `EGLDisplay`
- device 查询链路是通的
- D3D11 multithread protection 确实已经生效

这非常关键，因为它把工作假设从：

- “Qt 原生句柄 ABI 可能有问题”

转变为：

- “真正的问题是跨模块混用了 EGL 运行时”

因此，后续工作重点不应再停留在 ANGLE 句柄取证本身，而应转向未来 GPU 算法库接入时的运行时一致性加固。

## 剩余警告

成功验证运行中仍然看到了一些告警，但目前不是致命问题：

- `DXGI WARNING ... blt-model swap effects ...`
- `D3D11 WARNING: ID3D11DeviceContext::End ... QUERY_END_ABANDONING_PREVIOUS_RESULTS`

当前判断是：

- DXGI swap-effect 告警更像 Qt / ANGLE 呈现路径的次级问题，不是本次崩溃根因
- 剩余的 D3D11 query 告警在已验证运行中没有导致不稳定
- 除非将来接入真实算法库后这些警告再次与故障强相关，否则可以先作为次级清理项处理

## 集成约束

未来真实 GPU 算法库接入时，必须严格守住一个原则：

- 它必须运行在 Qt 创建出来的 worker 共享上下文上
- 它不能悄悄再拉起另一条 EGL / GLES / ANGLE 运行时路径

具体意味着：

- 不要再额外加载另一对 `libEGL` / `libGLESv2`
- 不要把 Qt 持有的 ANGLE handle 传给另一份模块实例中的 EGL API
- 如果库内部自己解析 GL / EGL 入口，解析目标必须是当前真正拥有上下文的运行时

## 相关文件

- `src/async_gles_widget.cpp`
- `src/async_gles_widget.h`
- `src/shared_texture_worker.cpp`
- `src/shared_texture_worker.h`
- `src/angle_threading.cpp`
- `src/angle_threading.h`
- `src/gles_thread_guard.cpp`
- `src/gles_thread_guard.h`
