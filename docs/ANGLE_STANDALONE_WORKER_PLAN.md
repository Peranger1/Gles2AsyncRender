# ANGLE Standalone Worker 方案

> 2026-05-10
>
> 本文档最初描述的是下一阶段计划实施的 worker 渲染隔离方案。
>
> 2026-05-11 补充：
>
> - worker standalone runtime 已落地
> - standalone GPU publish + CPU fallback 已落地
> - 第三阶段 UI `copy-on-acquire`、slot 状态收敛、worker 事件唤醒 已落地
>
> 当前本文档应理解为“实施计划 + 已落地状态说明”。
>
> 目标不是继续在 worker 中复用 Qt 的 `QOpenGLContext`，而是：
>
> - UI 侧继续使用 Qt 自己的 ANGLE / `QOpenGLWidget`
> - worker 侧不再使用 `QOpenGLContext` / `QOffscreenSurface`
> - worker 侧直接使用 `libEGL[d].dll` / `libGLESv2[d].dll` API
> - worker 侧自己创建第二个 `EGLDisplay`、第二套 `EGLContext`
> - 算法库和 worker publish 流程都只认 worker 自己的 GLES/EGL 函数表

## 1. 背景与问题定义

当前主分支已经把算法执行和显示解耦为：

- UI 线程通过 `D3D11ImportWidget` 显示 shared texture
- worker 线程生成结果并发布到 `D3D11NativeSlotPool`

但当前 worker 仍然存在一个关键问题：

- 历史路径中，`D3D11NativeWorker` 内部仍然创建了 `QOffscreenSurface + QOpenGLContext`
- 历史路径中，`PhotoEditorLibraryHost` 仍然通过 `QOpenGLContext::getProcAddress()` 向算法库提供函数入口
- 历史路径中，`AngleSharedTexturePublishBridge` 仍然要求 `libraryContext` current

这意味着：

- worker 侧仍然在使用 Qt 管理的 ANGLE runtime
- 算法库和 publish bridge 仍然可能落到与 UI 侧相同的 ANGLE renderer / D3D11 immediate context
- 即使逻辑上“不是共享 `QOpenGLContext`”，底层仍可能共享同一套 ANGLE D3D11 状态机

当前已观察到的典型崩溃/断言包括：

- `StateManager11.cpp:1992` `ASSERT(mInternalDirtyBits.none())`
- `DisplayImpl.cpp:24` `ASSERT(mState.surfaceSet.empty())`

这些现象说明当前路径仍不足以彻底隔离 UI 与 worker 的 ANGLE 内部状态。

## 2. 方案结论

推荐落地路径如下：

1. 第一、二阶段 UI 侧尽量保持不变。
2. worker 侧彻底不再创建 `QOpenGLContext`。
3. worker 侧新增一个独立的 `AngleStandaloneRuntime`。
4. `AngleStandaloneRuntime` 自己加载 Qt 当前进程已经加载的那一对 `libEGL[d].dll` / `libGLESv2[d].dll`。
5. `AngleStandaloneRuntime` 自己创建独立的 `ID3D11Device`。
6. worker 通过 `EGL_PLATFORM_DEVICE_EXT` 基于这台独立 `ID3D11Device` 创建新的 `EGLDisplay`。
7. worker 上的算法库初始化、算法渲染、publish bridge 全部只使用这套 standalone runtime 的函数表。

第一阶段只做：

- standalone ANGLE worker
- 算法渲染
- `glReadPixels + UpdateSubresource` 的 CPU publish

第一阶段不做：

- worker 侧 GPU import / `eglCreatePbufferFromClientBuffer`
- worker 侧零拷贝 publish

这是为了先把“崩溃原因隔离”和“上下文边界清理”做对，再考虑优化。

第二阶段再做：

- worker 侧 GPU import / `eglCreatePbufferFromClientBuffer`
- worker 侧零拷贝 publish
- 保留 CPU publish fallback

第三阶段再做：

- UI 侧 `copy-on-acquire`
- shared slot 在同一次 `paintGL()` 内立即归还
- worker 基于 slot release 事件唤醒，而不是 `scheduleRender(4)` 轮询

## 3. 设计目标

本方案的目标是：

- 让 worker 的 GLES/EGL 生命周期完全脱离 Qt 的 `QOpenGLContext`
- 保证算法库看到的函数指针全部来自同一套 standalone runtime
- 保证 worker 的 `EGLDisplay` 绑定到独立的 `ID3D11Device`
- 第一、二阶段保持 UI 侧 `D3D11ImportWidget` 显示链路尽量不变
- 第一、二阶段保持 `D3D11NativeSlotPool` 和 shared texture 交接模型尽量不变
- 第三阶段将 shared slot 从“持续显示中的 front 资源”收敛为“短生命周期传输缓冲”

第一阶段明确不追求：

- 第一阶段实现 worker 侧零拷贝 publish
- 修改 UI 侧显示架构
- 改造 `D3D11ImportWidget` 为非 Qt 路线

## 4. 当前边界与目标边界

### 4.1 当前边界

当前 worker 路径的关键事实：

- 历史路径中，`D3D11NativeWorker` 仍持有 `librarySurface` 和 `libraryContext`
- 历史路径中，`PhotoEditorLibraryHost` 的 resolver 仍来自 `QOpenGLContext::currentContext()`
- 历史路径中，`photo_editor_*` 模拟实现内部仍使用 `QOpenGLFunctions`
- 历史路径中，`AngleSharedTexturePublishBridge` 仍基于 `QOpenGLContext`

因此当前真实边界并不是“worker 独立 runtime”，而是“worker 使用另一个 Qt GL context”。

### 4.2 目标边界

目标边界应调整为：

- UI 侧
  - 继续使用 Qt / ANGLE
  - 继续使用 `QOpenGLWidget`
  - 只负责导入和显示 shared texture
- worker 侧
  - 不再依赖 `QOpenGLContext`
  - 不再依赖 `QOffscreenSurface`
  - 不再依赖 `QOpenGLFunctions`
  - 不再依赖 `QOpenGLShaderProgram`
  - 只依赖 standalone `libEGL/libGLESv2` + 原生 GLES2 API

## 5. 目标架构

### 5.1 UI 侧保持不变

UI 线程继续保留当前实现：

- `D3D11ImportWidget`
- `D3D11NativeSlotPool`
- shared texture 导入
- keyed mutex
- `eglBindTexImage` 读取共享纹理内容
- 四边形显示

这部分不应成为第一阶段改动重点。

### 5.2 worker 侧新结构

worker 侧调整为：

- `AngleStandaloneRuntime`
  - runtime 初始化
  - EGL/GLES 函数解析
  - 独立 `ID3D11Device`
  - `EGLDeviceEXT`
  - `EGLDisplay`
  - `EGLContext`
  - pbuffer surface
- `PhotoEditorLibraryHost`
  - 不再从 Qt context 解析函数
  - 改为从 `AngleStandaloneRuntime` 解析函数
- `photo_editor_*`
  - 不再使用 Qt GL 抽象
  - 只使用 standalone proc table
- 第一阶段：
  - `D3D11CpuPublishBridge`
  - 只保留 CPU publish
  - 从 standalone runtime 的当前 context 执行 `glReadPixels`
  - 用独立 D3D11 device 的 immediate context 执行 `UpdateSubresource`
- 第二阶段：
  - `D3D11StandalonePublishBridge`
  - 优先走 standalone runtime 内的 GPU publish
  - 如 GPU publish 初始化或运行失败，则回退到 CPU publish
- 第三阶段：
  - `D3D11ImportWidget`
    - 对 shared slot 执行 `copy-on-acquire`
    - 在同一次 `paintGL()` 中完成 `AcquireSync -> eglBindTexImage -> copy -> eglReleaseTexImage -> ReleaseSync`
    - 后续显示只采样 UI 本地 display texture
  - `D3D11NativeSlotPool`
    - 状态从 `Free -> Rendering -> Pending -> Front -> Retiring -> Free`
      收敛为 `Free -> Rendering -> Pending -> Free`
  - `D3D11NativeWorker`
    - 不再通过 `scheduleRender(4)` 轮询等待 free slot
    - 改为由 UI 在 slot 释放后发信号唤醒 worker

## 6. 新增模块建议

### 6.1 `src/angle_standalone_runtime.h/.cpp`

建议新增统一 runtime 封装，职责包括：

- 识别 Qt 当前进程已经加载的 `libEGLd.dll/libEGL.dll`
- 识别 Qt 当前进程已经加载的 `libGLESv2d.dll/libGLESv2.dll`
- 固定 worker 使用的模块句柄与路径
- 解析：
  - `eglGetProcAddress`
  - `eglCreateDeviceANGLE`
  - `eglReleaseDeviceANGLE`
  - `eglGetPlatformDisplayEXT`
  - `eglInitialize`
  - `eglChooseConfig`
  - `eglCreatePbufferSurface`
  - `eglCreateContext`
  - `eglMakeCurrent`
  - `eglDestroyContext`
  - `eglDestroySurface`
  - `eglTerminate`
- 创建独立 `ID3D11Device`
- 用 `eglCreateDeviceANGLE(EGL_D3D11_DEVICE_ANGLE, ...)` 创建 `EGLDeviceEXT`
- 用 `eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, ...)` 创建 `EGLDisplay`
- 创建 worker 专用 pbuffer + context
- 提供：
  - `initialize(QString *error)`
  - `makeCurrent(QString *error)`
  - `doneCurrent(QString *error)`
  - `void *resolveProc(const char *name) const`
  - `RendererIdentity queryRendererIdentity() const`

### 6.2 `src/gles2_proc_table.h/.cpp`

建议新增统一函数表，职责包括：

- 宿主在初始化时一次性解析并缓存 worker 所需 GLES2/EGL 入口
- 算法库和 publish bridge 共用同一张表

建议至少覆盖：

- 纹理、FBO、shader、program、viewport、clear、draw、readback 相关 GLES2 API
- 必要的 EGL API

### 6.3 `src/gles2_shader_utils.h/.cpp`

建议新增轻量 shader 工具，替代 `QOpenGLShaderProgram`：

- 编译 vertex shader / fragment shader
- link program
- 打印 shader / program log

## 7. 现有模块的改造建议

### 7.1 `src/d3d11_native_worker.cpp`

需要做的改动：

- 删除：
  - `std::unique_ptr<QOffscreenSurface> librarySurface`
  - `std::unique_ptr<QOpenGLContext> libraryContext`
- 新增：
  - `std::unique_ptr<AngleStandaloneRuntime> algorithmRuntime`
- 所有 `photo_editor_create/destroy/process/render` 调用前后：
  - 改为 `algorithmRuntime->makeCurrent()`
  - 完成后 `algorithmRuntime->doneCurrent()`
- 初始化顺序改为：
  1. 创建 `AngleStandaloneRuntime`
  2. 初始化 runtime
  3. `PhotoEditorLibraryHost::initializeOnce(runtime, ...)`
  4. 初始化 standalone publish bridge
  5. 进入正常 worker 生命周期

### 7.2 `src/photo_editor_library_host.h/.cpp`

需要做的改动：

- `initializeOnce(QOpenGLContext *libraryContext, QString *error)`
  - 改为 `initializeOnce(AngleStandaloneRuntime *runtime, QString *error)`
- resolver 不再走：
  - `QOpenGLContext::currentContext()->getProcAddress(name)`
- resolver 改为走：
  - `runtime->resolveProc(name)`

同时保留现有的“全局只初始化一次”约束：

- `photo_editor_init` 继续只调用一次
- 但这一轮初始化绑定的是 standalone runtime 的 resolver

### 7.3 `src/photo_editor_gles2_simulator.h/.cpp`

需要做的改动：

- 删除对以下 Qt GL 类的依赖：
  - `QOpenGLContext`
  - `QOpenGLFunctions`
  - `QOpenGLShaderProgram`
- 保留以下 Qt 非 GL 类型是可以接受的：
  - `QImage`
  - `QSize`
  - `QString`
  - `QTimer`
  - `QObject`

所有渲染逻辑改为：

- 从 `Gles2ProcTable` 取函数
- 用原生 GLES2 API 完成：
  - shader 编译/link
  - texture 创建
  - FBO 创建
  - draw
  - `glReadPixels`

### 7.4 第一阶段 publish bridge

第一阶段建议不要保留这个名字和 GPU import 分支。

建议改造为：

- `src/d3d11_cpu_publish_bridge.h/.cpp`

职责调整为：

- 接收算法库输出的 `textureId + size`
- 在 worker runtime 当前 context 下执行 `glReadPixels`
- 把 RGBA 转成 BGRA
- 通过独立 D3D11 device 的 `UpdateSubresource` 上传到 slot texture
- keyed mutex 交给 UI 侧继续消费

第一阶段明确删除或禁用：

- `eglCreatePbufferFromClientBuffer`
- `eglQuerySurfacePointerANGLE`
- worker 侧 `eglBindTexImage`
- worker 侧 imported-slot GPU publish

### 7.5 第二阶段 publish bridge

第二阶段在第一阶段稳定后，将 publish bridge 继续收口为：

- `src/d3d11_standalone_publish_bridge.h/.cpp`

职责调整为：

- 接收算法库输出的 `textureId + size`
- 优先在 standalone runtime 当前 context 下执行 GPU publish：
  - `eglCreatePbufferFromClientBuffer`
  - `eglBindTexImage`
  - FBO copy pass
  - `eglReleaseTexImage`
- 若 standalone ANGLE D3D texture import 路径不可用或运行失败：
  - 回退到 `glReadPixels + UpdateSubresource` 的 CPU publish
- keyed mutex 继续交给 UI 侧消费

## 8. 函数指针一致性保证

这是本方案最关键的约束之一。

### 8.1 必须满足的前提

“函数指针一致”不是指 UI 和 worker 共享同一个 `QOpenGLContext`。

真正需要保证的是：

- worker 在初始化时只选定一对固定的 `egl/gles` 模块
- worker 后续所有 GL/EGL 入口都只从这对模块解析
- 算法库和 publish bridge 使用的是同一张 proc table

### 8.2 建议的解析顺序

`AngleStandaloneRuntime::resolveProc(name)` 建议按如下顺序解析：

1. `GetProcAddress(glesModule, name)`
2. `GetProcAddress(eglModule, name)`
3. `eglGetProcAddress(name)`

解析成功后：

- 将函数地址写入 `Gles2ProcTable`
- 后续所有调用都只走 `Gles2ProcTable`

### 8.3 必须避免的错误做法

以下做法会破坏一致性：

- 在 worker 中继续使用 `QOpenGLContext::getProcAddress()`
- 在算法库内部继续使用 `QOpenGLFunctions`
- 一部分函数来自 standalone runtime，另一部分函数来自 Qt context
- 运行时显式再加载另一份不一致的 `libEGL/libGLESv2`

## 9. 如何保证 UI 与 worker 不在同一个 ANGLE renderer

### 9.1 不能只看 `EGLDisplay` 句柄

单纯“拿到不同 `EGLDisplay` 指针”并不能充分证明 renderer 隔离。

真正要看的，是 `EGLDisplay` 背后的 `ID3D11Device`。

### 9.2 正确做法

worker 必须：

1. 自己调用 `D3D11CreateDevice`
2. 用这台 device 创建 `EGLDeviceEXT`
3. 通过 `EGL_PLATFORM_DEVICE_EXT` 获取 `EGLDisplay`

即：

- worker display 由 worker 自己的 `ID3D11Device` 驱动
- UI display 仍由 Qt 自己的 ANGLE device 驱动

### 9.3 运行时验证

建议运行时同时打印：

- UI 侧：
  - `EGLDisplay`
  - `EGL_DEVICE_EXT`
  - `EGL_D3D11_DEVICE_ANGLE`
  - adapter LUID
- worker 侧：
  - `EGLDisplay`
  - `EGL_DEVICE_EXT`
  - `EGL_D3D11_DEVICE_ANGLE`
  - adapter LUID

判定规则：

- adapter LUID 可以相同
  - 说明两边在同一块物理 GPU 上
- `ID3D11Device *` 不能相同
  - 说明两边不在同一个 ANGLE renderer / D3D11 immediate context 上

## 10. 第一阶段实施顺序

推荐按下面顺序实施：

1. 新增 `AngleStandaloneRuntime`
2. 补齐 standalone EGL/GLES 解析与独立 device/display/context 初始化
3. 新增 `Gles2ProcTable`
4. 改造 `PhotoEditorLibraryHost`
5. 改造 `photo_editor_gles2_simulator`
6. 将 `D3D11NativeWorker` 从 `QOpenGLContext` 路线切到 standalone runtime
7. 将 publish bridge 收敛为 CPU publish 版本
8. 保持 UI 侧不变，做完整回归

不要一开始就同时做：

- standalone runtime
- worker GPU import publish
- UI 侧重构

这样会让问题边界再次混在一起。

## 11. 第二阶段与第三阶段可选优化

如果第一阶段稳定，再考虑第二阶段：

- 重新引入 worker 侧 GPU publish
- 但该 GPU publish 也必须基于 standalone runtime 自己的 EGL/GLES
- 不能再次把 publish 流程拉回 Qt `QOpenGLContext`

第二阶段开始前，必须先确认：

- 第一阶段已稳定运行
- 所有算法库渲染和资源释放路径都不再依赖 Qt context
- worker 与 UI 的 `ID3D11Device` 已明确分离

### 11.1 当前第二阶段实施方向

当前第二阶段开始后的收口目标调整为：

- worker publish bridge 改为 `D3D11StandalonePublishBridge`
- 优先走 standalone runtime 内的 GPU publish：
  - `eglCreatePbufferFromClientBuffer`
  - `eglBindTexImage`
  - FBO copy pass
  - `eglReleaseTexImage`
- 保持 `D3D11NativeSlotPool`、shared handle、keyed mutex、UI import widget 不变
- 如 standalone runtime 上的 ANGLE D3D texture import 失败，则允许回退到 CPU publish 兜底

第二阶段当前不做：

- UI 侧 import widget 重构
- worker / UI 统一到同一 `EGLDisplay`
- 去掉 CPU fallback

### 11.2 第三阶段实施方向

第三阶段的目标不是继续压榨 worker 侧 publish bridge，而是缩短 UI 对 shared slot 的占用窗口。

当前第二阶段实现中，UI 侧会在首次读取 `front slot` 后持续持有 imported texture 与 keyed mutex，
直到该 slot 退役并在 `frameSwapped()` 后才释放。

这会带来两个问题：

- shared slot 被长期占用，worker 可复用 slot 数下降
- worker 在没有 free slot 时只能靠定时轮询重试

第三阶段建议将 shared slot 的职责改为：

- 只负责 worker 到 UI 的跨 runtime 传输
- 不再承担“持续显示中的 front 纹理”职责

第三阶段建议的 UI 侧路径如下：

1. `onFrameReady()` 只记录 `pending slot` 元数据并调用 `update()`
2. `paintGL()` 中检测到 `pending slot` 后：
   - `ensureImportedSlot(slotIndex)`
   - `AcquireSync(1, short_timeout)`
   - `eglBindTexImage()`
   - 将 imported shared texture 复制到 UI 自己的 `display texture` / `display FBO`
   - `glFlush()`
   - `eglReleaseTexImage()`
   - `ReleaseSync(0)`
   - 将该 slot 立即归还到 `Free`
3. 后续显示路径只采样 UI 本地 `display texture`
4. 如果本次没有新 `pending slot`，则继续显示上一帧的本地 `display texture`

第三阶段建议的 slot pool 语义如下：

- shared slot 不再保留 `Front` / `Retiring`
- shared slot 状态收敛为：
  - `Free`
  - `Rendering`
  - `Pending`
- 生命周期收敛为：
  - `Free -> Rendering -> Pending -> Free`

这意味着：

- worker publish 完成后，slot 进入 `Pending`
- UI copy 成功并释放 keyed mutex 后，slot 立即回到 `Free`
- `frameSwapped()` 不再承担 shared slot 回收职责

第三阶段建议的 worker 唤醒方式如下：

- 当 worker 因无 free slot 而无法继续 publish 时，不再使用 `scheduleRender(4)` 轮询
- UI 在成功完成 local copy 并释放 slot 后，发出类似 `slotAvailableForWorker()` 的信号
- worker 收到该信号后：
  - 若当前正等待 free slot
  - 且已有 `renderReady` 结果
  - 则立即 `scheduleRender(0)` 继续 publish

第三阶段的收益是：

- shared slot 占用时间从“一个显示周期”缩短到“一次 copy pass”
- worker 吞吐不再直接受 `frameSwapped()` 节奏约束
- slot pool 的状态机与资源职责更加一致

第三阶段当前仍不建议做：

- worker / UI 合并到同一个 `EGLDisplay`
- 去掉 keyed mutex
- 在第三阶段同时引入新的跨进程或跨 API fence 协议

## 12. 验收标准

第一阶段完成后，应满足：

- worker 日志中不再出现 `QOpenGLContext` / `QOffscreenSurface` 作为算法执行路径
- 算法库 resolver 不再来自 `QOpenGLContext::getProcAddress()`
- `photo_editor_*` 不再依赖 `QOpenGLFunctions` / `QOpenGLShaderProgram`
- UI 侧仍可正常显示 shared texture
- 连续 brightness/contrast/zoom 调整时：
  - 不再触发 `StateManager11.cpp:1992`
  - 不再触发 `DisplayImpl.cpp:24`
- UI 与 worker 查询到的 `ID3D11Device *` 不相同

### 12.1 当前第一阶段收口状态

截至 2026-05-10，第一阶段收口后的历史实现约束如下：

- worker 侧仅保留 `AngleStandaloneRuntime + D3D11CpuPublishBridge`
- 仓库内不再保留旧的 `AngleSharedTexturePublishBridge` Qt-context publish 路线
- 默认运行日志只保留初始化、错误、关键状态切换
- 高频运行时诊断日志默认关闭
- 如需排查运行时细节，可设置环境变量：
  - `GLES2ASYNC_DIAG=1`
- 关闭窗口路径已加入 shutdown 保护，避免 teardown 阶段继续接收新的 render/progress 事件
- 对当前 Qt 5.15.1 自带 ANGLE 路径：
  - `EGL_PLATFORM_DEVICE_EXT` 对应的 standalone runtime shutdown 中，不应显式调用 `eglReleaseDeviceANGLE`
  - 原因是该 ANGLE 构建会在 `Display::~Display()` 中再次删除同一 `Device`
  - 若显式 release，会触发 `libANGLE/Device.cpp` 的 double-destroy 断言

建议将以下场景作为第一阶段最小回归集：

- 导入图片
- 连续拖动 brightness / contrast / zoom
- 窗口 resize
- 关闭窗口

### 12.2 当前第二阶段验证重点

第二阶段开始后，最小验证重点调整为：

- worker 是否成功打印 standalone publish bridge 的初始模式、初始路径和原因
- 导入图片后首帧是否正常显示
- 连续拖动 brightness / contrast / zoom 时是否仍稳定
- resize 后 shared texture 是否仍可持续更新
- 关闭窗口时不再引入新的 teardown 崩溃

若 GPU publish 初始化失败或运行时回退，日志中应明确给出 fallback 到 CPU publish 的原因。

### 12.3 当前第二阶段收口状态

截至 2026-05-10，第二阶段当前实现约束如下：

- worker 侧 publish bridge 已切换为 `AngleStandaloneRuntime + D3D11StandalonePublishBridge`
- 默认发布模式为 `auto`
  - 优先走 standalone runtime 内的 GPU publish
  - 通过环境变量 `GLES2ASYNC_PUBLISH_MODE=force_cpu` 可强制走 CPU fallback
- bridge 初始化时会打印：
  - `requestedMode`
  - `initialPath`
  - ANGLE D3D texture import 入口是否齐备
  - 当前 `EGLDisplay`
  - 当前 `EGLConfig`
  - 当前选路原因
- 若运行时发生 GPU publish -> CPU fallback：
  - 日志会明确打印 fallback 原因
- 当前仍保留 CPU fallback 作为正式兜底能力

### 12.4 第三阶段验证重点

第三阶段开始后，最小验证重点建议调整为：

- UI 在消费 `pending slot` 的同一次 `paintGL()` 中完成：
  - `AcquireSync`
  - `eglBindTexImage`
  - local copy
  - `eglReleaseTexImage`
  - `ReleaseSync`
- shared slot 不再跨多个显示周期保持 `front` 占用
- worker 在无 free slot 时不再通过 `scheduleRender(4)` 轮询重试
- UI 成功归还 slot 后，worker 能被事件直接唤醒并继续 publish
- 即使 `frameSwapped()` 节奏偏慢，worker 吞吐也不再被其直接卡住
- 当某次 local copy 失败时，UI 仍能继续显示上一帧本地 `display texture`

## 13. 风险与注意事项

### 13.1 不能显式链接另一份 EGL/GLES

worker 不应在工程里直接链接另一份 `libEGL/libGLESv2` import lib。

正确方式是：

- 使用 Qt 当前进程已经加载的同一对 DLL
- 但 worker 的 display/context/device 生命周期独立

### 13.2 `photo_editor_init` 的全局一次性约束

当前 `photo_editor_init` 是进程级全局初始化。

这意味着：

- 一旦 resolver 绑定到 standalone runtime，就必须保证后续算法库也只使用这套 runtime
- 不应再出现第二套宿主 resolver 混入

### 13.3 CPU publish 是刻意保守的第一阶段选择

第一阶段可能会比 GPU publish 慢，但这是有意为之：

- 先做边界正确
- 再做性能优化

## 14. 实施建议总结

本方案的核心不是“把 `QOpenGLContext` 替换成 `eglCreateContext`”这么简单。

真正需要同时完成的是：

- runtime ownership 切换
- 函数入口切换
- 算法库内部 GL 抽象切换
- publish bridge 切换

建议的新对话实施策略是：

1. 先只创建 standalone runtime 和 proc table
2. 再切 `PhotoEditorLibraryHost`
3. 再切 `photo_editor_gles2_simulator`
4. 再切 `D3D11NativeWorker`
5. 第一阶段先把 publish bridge 收敛为 CPU publish
6. 第二阶段再把 publish bridge 切到 standalone GPU publish，并保留 CPU fallback
7. 第三阶段再做 UI `copy-on-acquire`、slot pool 状态收敛、worker 事件驱动唤醒

这样每一步都有清晰回归边界，便于快速确认问题是否真正被隔离。
