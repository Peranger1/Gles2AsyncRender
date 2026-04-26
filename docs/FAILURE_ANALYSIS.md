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

截至大约 `2026-04-24 01:16` 的验证结果，样例已经可以：

- 正常启动
- 正常渲染
- resize 后不再黑屏
- 正常退出

日志同时确认：

- widget 上下文和 worker 上下文解析到同一个 ANGLE `EGLDisplay`
- `ID3D11Multithread` 保护已经成功启用
- 之前出现的 `D3D11 CORRUPTION` 崩溃在本次验证运行中已消失

到了 `2026-04-26`，又继续暴露出两类更细的后续问题：

- 启动阶段偶发的 worker `makeCurrent()` 失败，日志表现为 `eglError: 3006`
- resize 相关的显示伪影，从“瞬间透明”进一步演化到“缩放期间持续透明”

这两类问题已经不再属于最初的 ANGLE/D3D11 多线程损坏，而是：

- 启动时序与 `QOpenGLWidget` 内部初始化阶段的竞态
- 共享纹理提交协议和 resize 期间纹理存储管理的问题

目前代码已经为这两类问题引入了新的收敛路径：

- worker 启动延后到首帧 `frameSwapped()` 之后
- 显示侧改为 `front / pending / retiring / free` 槽位协议
- worker 侧改为只对当前 render slot 做纹理重分配，避免 resize 时破坏正在显示的 front texture

到了 `2026-04-26` 晚些时候，样例又从“最小共享纹理验证程序”演进成了一个更接近真实接入方式的图像处理 demo：

- UI 侧新增 `MainWindow`
- worker 改成 `QObject + moveToThread`
- 支持导入图片目录
- worker 上下文内执行亮度、对比度、缩放、平移、旋转和翻转
- 输出图像改为按宽高比显示

这轮演进又暴露出几类新的应用层问题：

- 导入图片目录时报错 `Source image upload prerequisites are not ready.`
- 目录加载成功但 widget 没有显示任何图片
- 图像初版显示为强制拉伸铺满 widget，不符合图像处理预览场景

这些问题已经不再属于底层 ANGLE/D3D11 多线程损坏，而是：

- worker 图像上传时机问题
- 共享纹理槽位注册时序问题
- 输出几何计算策略不符合图像预览需求

最新代码已在 `2026-04-26` 使用 `vcvars64.bat + qmake + nmake` 编译通过。最终的 live-resize 观感仍应以最新运行验证结果为准。

## 当前结论

最终确认的结论是：

- 当前目标架构本身可行
- 共享纹理设计不是崩溃根因
- 真正决定性的根因是 ANGLE 补丁路径中曾经把 Qt 持有的 EGL 句柄和另一份模块实例中的 EGL 入口混用了
- 当 EGL 函数改为从 Qt 实际加载的 ANGLE 模块中解析，并启用 `ID3D11Multithread` 之后，样例在验证运行中变得稳定

此外，后续又确认了两点：

- 启动阶段的 `eglError: 3006` 不是新的 ANGLE/D3D11 底层损坏，而是 worker 共享上下文在 `QOpenGLWidget` 首帧初始化尚未完全稳定时过早 `makeCurrent()`
- resize 期间的透明伪影不是共享上下文本身失效，而是显示协议和纹理重分配策略还不够严格
- 在当前 demo 形态下，后续出现的目录加载失败和“加载成功但不显示”都属于应用层资源准备顺序错误，而不是共享上下文模型失效

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

### 7. 新出现的启动失败：worker `makeCurrent()` 返回 `eglError: 3006`

后续运行中观察到的新日志：

- `QWindowsEGLContext::makeCurrent: Failed to make surface current. eglError: 3006`
- `Shared texture worker error: "Failed to make worker context current."`

这次故障与之前的 `D3D11 CORRUPTION` 不同，特点是：

- worker 上下文已经创建成功
- ANGLE threading patch 也已经生效
- 但 worker 第一次在离屏 surface 上 `makeCurrent()` 时，底层返回 `EGL_BAD_CONTEXT (0x3006)`

根因分析：

- 这更像是启动时序问题，而不是底层 D3D11 immediate context 再次并发损坏
- worker 线程原先是在 `initializeGL()` 里立即启动
- 此时 `QOpenGLWidget` 自己的首帧初始化、内部 FBO 和离屏 surface 状态还处于过渡阶段
- 过早让 worker 抢占共享上下文路径，会让 ANGLE/Qt 的离屏 `makeCurrent()` 组合变得不稳定

采取的修改：

- 不再在 `initializeGL()` 中立刻启动 worker
- 改为等首帧 `frameSwapped()` 之后再启动 worker
- 同时在 worker 的 `makeCurrent()` 路径中加入有限重试和更详细的上下文 / offscreen surface 状态日志

结果：

- 启动路径重新恢复稳定
- 这一步把问题从“后端线程损坏”切换为“首帧初始化时序竞态”并成功规避

### 8. 启动稳定后，resize 出现“瞬间透明”

在后续验证中，窗口缩放不再崩溃，但显示路径又暴露出新的现象：

- 不是黑屏
- 也不是立即崩溃
- 而是在 resize 期间出现“瞬间透明”

根因分析：

- `QOpenGLWidget` 在 resize 时会重建内部 FBO，并清掉旧尺寸内容
- 当前 widget 侧使用 `NoPartialUpdate`
- worker 仍以异步节流方式生成新尺寸纹理
- 因此 resize 过程中会存在一个短暂空窗：
  - widget 已经切到新尺寸 FBO
  - worker 还没有提交第一张新尺寸的可显示纹理
- 在这个空窗里，顶层窗口合成时会短暂暴露出父窗口/背景，看起来就像 widget 区域透明了一下

采取的修改方向：

- 不再采用“谁最新就直接显示谁”的弱约束纹理切换
- 开始收敛到显式的 front/back 提交协议

### 9. 第一次 front/back 收敛后，现象从“瞬间透明”变成“缩放期间持续透明”

第一次引入 front/back 提交协议后，观察到新的表现：

- resize 时不再只是瞬间透明
- 而是在整个 live resize 期间持续透明
- 停止缩放后才重新显示

这个阶段非常关键，因为它说明：

- front/back 提交协议的总体方向是对的
- 但纹理生命周期管理仍然有漏洞

第一次 front/back 方案的设计要点是：

- worker 使用多槽位共享纹理池
- UI 只显示当前 `front`
- worker 只把新帧提交成 `pending`
- UI 在 `paintGL()` 中把 `pending` 升格为新的 `front`
- 旧 `front` 等到 `frameSwapped()` 后再回收

第一次实现后的根因分析：

- 虽然 worker 不再直接渲染到当前 `front` 对应的槽位
- 但 resize 时 worker 仍然会对所有槽位统一执行 `glTexImage2D()`
- 这会把当前正在显示的 `front texture` 的底层存储也一起重分配
- 于是：
  - UI 逻辑上还在“继续显示旧 front”
  - 但这个旧 front 的实际纹理存储已经被重置
- 最终表现就是：整个 live resize 期间都没有可稳定显示的 front 内容

采取的修正：

- 保留 `front / pending / retiring / free` 提交协议
- 但把纹理分配策略改为：
  - 只对当前 render slot 单独重分配
  - 不再在 resize 时批量重分配所有共享纹理
- 这样当前正在显示的 `front` 纹理在新尺寸帧真正提交前不会被改写

结果：

- 显示协议从“弱约束的直接切换”升级为“显式提交 + 延迟回收”
- 纹理存储从“全槽位统一 resize”收紧为“按 render slot 单独 resize”
- 这一步是解决 live resize 透明伪影的关键结构修正
- 最新代码已编译通过，运行时最终效果仍以用户最新验证为准

### 10. 样例重构为图像处理 demo 后，worker 架构从“线程内自管”收敛为 `QObject + moveToThread`

这轮调整不是为了修一个单点 crash，而是为了让样例结构更接近未来真实算法库接入形态。

旧的最小验证程序更像“线程 + widget + 共享纹理”的直接拼装，而新的结构调整为：

- `MainWindow` 负责 UI、菜单、状态栏、参数面板和 worker 线程生命周期
- `SharedTextureWorker` 只保留为 worker object
- worker object 被 `moveToThread()` 到独立 `QThread`
- 显示初始化、worker 初始化、目录加载和重绘请求都通过 Qt 信号槽串接

这样做的原因是：

- 未来真实 GPU 算法库不会只暴露一个“单帧渲染函数”
- 它通常需要目录/资源加载、一次性初始化、参数更新、状态切换和逐帧执行
- 这些动作更适合落在稳定的 worker object 生命周期上，而不是继续塞进零散的演示线程逻辑

这一轮不是为了解决 ANGLE 崩溃本身，但它为后续图像处理 demo 的问题暴露和定位创造了更清晰的边界。

### 11. 导入图片目录时报错 `Source image upload prerequisites are not ready`

在引入图片目录加载后，用户第一次验证时弹出错误：

- `Source image upload prerequisites are not ready`

根因分析：

- `ImageProcessingPipeline::loadImageDirectory()` 在加载首张图片后，直接走到了上传路径
- 但此时 `m_sourceTexture` 还没有通过 `ensureSourceTexture()` 建立
- 于是 `uploadCurrentImage()` 看到的前置条件不满足：
  - `m_gl == nullptr` 或
  - `m_sourceTexture == 0` 或
  - 当前图像为空
- 最终把“纹理尚未创建”的问题报成了“上传前置条件不满足”

采取的修改：

- 在 `loadImageDirectory()` 末尾不再直接依赖后续渲染阶段隐式建纹理
- 改为先显式调用 `ensureSourceTexture(context, error)`

结果：

- 目录导入路径开始具备自洽的首图上传流程
- 这类失败被收敛为真正的图片读取或 GL 上传错误，不再因为源纹理未初始化而提前中断

### 12. 目录加载成功但没有显示图片

修掉首图上传前置条件问题后，下一轮又出现：

- 没有弹窗报错
- 目录也能正常加载
- 但 widget 区域没有显示图片

根因分析：

- 当前显示协议要求 worker 必须先拿到一个可用 render slot，才能把新帧提交成 `pending`
- `SharedTextureFramePool::tryAcquireRenderSlot()` 只会返回那些已经注册了有效 `textureId` 的槽位
- 但那一版代码里，共享纹理是在 worker 真正拿到 render slot 之后才懒创建
- 这就形成了循环依赖：
  - 没有注册纹理，就拿不到 render slot
  - 拿不到 render slot，就永远走不到创建并注册纹理的代码

采取的修改：

- 在 `SharedTextureWorker::initialize(...)` 阶段一次性为全部 worker 槽位创建共享纹理
- 初始化完成后立刻调用 `m_framePool->registerTexture(i, textureId)`
- 然后再 `reset()` 槽位状态机

结果：

- worker 第一次请求渲染时就能拿到有效 render slot
- 目录加载完成后，首帧可以真正进入 `pending -> front` 提交流程
- 这说明“不显示”问题来自槽位注册顺序，而不是共享上下文或 shader/FBO 本身失效

### 13. 图像显示方式从“强制铺满”收敛为“按宽高比显示”

在图片能够正常显示后，demo 仍有一个明显问题：

- 输出图像被直接铺满整个 widget
- 对图像处理预览场景来说，这会引入非算法本身的几何畸变

根因分析：

- 初版 `ImageProcessingPipeline::updateGeometry(...)` 相当于默认把内容 quad 固定铺到完整 NDC
- 这适合“全屏贴图显示”示例，不适合“图像预览”型 demo
- 一旦源图和输出纹理宽高比不一致，就会被拉伸

采取的修改：

- 在 `updateGeometry(...)` 中显式比较：
  - 源图宽高比
  - 输出纹理宽高比
- 按 aspect-fit 规则计算基础 quad 的半宽和半高
- 之后再叠加：
  - zoom
  - pan
  - rotation
  - flip

结果：

- 图像预览不再被默认拉伸
- 当前 demo 的几何行为更接近真实图像处理界面
- 后续若接入真实算法库，显示结果也不会再混入“显示层强制铺满”带来的假畸变

## 为什么当前互斥锁仍然不是万能解

当前互斥锁只能保护应用显式执行的 GL 代码区段，它无法完全控制：

- `QOpenGLWidget` 的内部合成行为
- ANGLE 自己的 D3D11 query
- Qt 或 ANGLE 内部触发的后端同步逻辑

这也是为什么即使应用层显式 GL 区段已经串行化，仍可能观察到 `GetData` 或 `ClearRenderTargetView` 这类后端损坏。

## 当前代码结构状态

当前代码被有意保留为未来真实 GPU 算法库接入所需的结构：

- `src/main_window.cpp`
  - 持有主窗口 UI
  - 持有 worker `QThread`
  - 在显示首帧 `frameSwapped()` 之后触发 worker 初始化
  - 负责目录导入、参数下发、状态展示和重绘请求

- `src/async_gles_widget.cpp`
  - 持有显示侧 `QOpenGLWidget`
  - 持有显示侧 front/pending 状态
  - 只在安全时机切换新的 front texture
  - 旧 front 会在 `frameSwapped()` 后再回收

- `src/shared_texture_worker.cpp`
  - 作为 worker object 被移动到独立 `QThread`
  - 使用 `SharedGlContextHandle` 持有共享 GLES2 上下文和离屏 surface
  - 在 worker 上下文内运行图像处理 demo 管线
  - 初始化阶段就为全部共享槽位创建并注册纹理
  - 只对当前 render slot 渲染
  - 把新帧先提交为 pending，再交给 widget 提升为 front

- `src/shared_gl_environment.cpp`
  - 从显示上下文派生共享环境
  - 创建 worker 使用的共享上下文和 `QOffscreenSurface`

- `src/shared_gl_context_handle.cpp`
  - 封装 worker 侧 `makeCurrent()` / `doneCurrent()` 生命周期

- `src/image_processing_pipeline.cpp`
  - 负责图片目录扫描和首图加载
  - 负责源图上传、FBO 输出、图像效果 shader 和 aspect-fit 几何

- `src/shared_texture_frame_pool.h`
  - 定义共享纹理槽位池
  - 显式管理 `free / rendering / pending / front / retiring` 状态

- `src/angle_threading.cpp`
  - 探测 Qt 暴露的 EGL native handle
  - 尝试定位 ANGLE 背后的 D3D11 device
  - 尝试启用 `ID3D11Multithread`

这套形态在继续调试时应尽量保持不变，因为它已经对齐未来真实库的承载模型。

## 根因归类总结

当前问题实际上分成两类，不应混为一谈：

1. 应用层资源与生命周期问题
   - 例如 resize 黑屏、启动阶段 `eglError: 3006`、front texture 被错误重分配、目录加载时源纹理未创建、共享槽位注册顺序错误
   - 这些问题可以在样例代码内部修复

2. 后端结构性线程冲突
   - 例如 `ClearRenderTargetView` / `GetData` 上的 `D3D11 CORRUPTION`
   - 这是 Qt 5.15.2 + ANGLE + `QOpenGLWidget` + worker 线程 GLES 的特定组合问题

3. 显示提交协议问题
   - 例如 resize 瞬间透明、live resize 持续透明
   - 这类问题不再是“能不能共享上下文”的问题，而是“前台显示纹理何时切换、何时回收、哪些槽位允许 resize”

在当前样例里，第 2 类问题已经不再是主要未解阻塞项；后续工作重点逐步转向第 1 类和第 3 类问题的工程化收敛。

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

### 策略 D：显示路径采用显式提交协议，而不是“最新帧直接覆盖显示帧”

这条策略是在解决 resize 透明伪影时新增的。

当前结论是：

- 如果 worker 可以直接改写 UI 正在显示的纹理
- 或 resize 时会批量重分配所有共享纹理

那么即使上下文共享和 ANGLE 线程保护都没有问题，live resize 期间仍然会出现显示伪影。

因此显示路径必须收敛到下面的规则：

- worker 只能渲染到当前拿到的 render slot
- 新帧只能先提交成 `pending`
- UI 只能在自己的显示时机把 `pending` 升格为新的 `front`
- 旧 `front` 只能在一次真正的 `frameSwapped()` 之后回收
- resize 时只能重分配当前 render slot，不能重分配正在显示的 `front`

## 已记录下来的关键现象

本轮排查已经明确记录过以下关键现象：

- 启动时出现 `D3D11 CORRUPTION: ClearRenderTargetView`
- 后续出现 `D3D11 CORRUPTION: GetData`
- resize 后黑屏并伴随共享纹理分配失败
- 后续出现 `QWindowsEGLContext::makeCurrent ... eglError: 3006`
- worker 报错 `Failed to make worker context current`
- 启动时序调整后，resize 期间出现“瞬间透明”
- 第一次 front/back 收敛后，现象变成“缩放期间持续透明，停止后恢复显示”
- 图像目录导入时报错 `Source image upload prerequisites are not ready`
- 目录加载成功但 widget 没有显示图片
- 图像显示初版为强制铺满，随后收敛为按宽高比显示
- `eglGetCurrentDisplay` 返回 `EGL_NO_DISPLAY`
- `eglQueryDisplayAttribEXT(EGL_DEVICE_EXT)` 返回 EGL 错误 `0x3008`
- `nativeResourceForContext("egldisplay")` 虽返回非空指针，但无法被 `eglQueryString` 接受
- `nativeResourceForIntegration("egldisplay")` 返回空
- `nativeResourceForWindow("egldisplay")` 被 `QWindowsNativeInterface` 拒绝
- 在改为从 Qt 已加载的 `libEGLd.dll` 解析 EGL 函数并启用 `ID3D11Multithread` 后，最终验证运行成功
- 在引入显式 front/pending/retiring 协议并把纹理重分配收紧为“只作用于当前 render slot”后，代码路径已经对准 live resize 伪影的真正根因

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

- `src/main_window.cpp`
- `src/main_window.h`
- `src/async_gles_widget.cpp`
- `src/async_gles_widget.h`
- `src/image_processing_pipeline.cpp`
- `src/image_processing_pipeline.h`
- `src/shared_gl_environment.cpp`
- `src/shared_gl_environment.h`
- `src/shared_gl_context_handle.cpp`
- `src/shared_gl_context_handle.h`
- `src/shared_texture_frame_pool.h`
- `src/shared_texture_worker.cpp`
- `src/shared_texture_worker.h`
- `src/angle_threading.cpp`
- `src/angle_threading.h`
- `src/gles_thread_guard.cpp`
- `src/gles_thread_guard.h`
