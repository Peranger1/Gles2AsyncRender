# Gles2AsyncRender Architecture

## 概述

当前仓库已经收敛到四块主结构：

- `framework/platform`
  - worker runtime
  - shared texture reader / writer
  - 平台 backend 总入口
- `framework/execution`
  - runtime host
  - GL task 执行器
  - 可选 lane 调度
  - sync / async 调用基础设施
- `app`
  - `QOpenGLWidget`
  - UI 显示与业务 glue
- `photo_editor`
  - `photo_editor_gles2_backend`
  - photo editor 结果类型

当前实现不再保留旧的 `RequestDispatcher`、`RequestChannel`、`IRequestHandler`、`photo_editor_demo_handlers` 主链路。

## 当前分层

### `src/framework/platform`

- 公共合同
  - `runtime.h`
  - `reader.h`
  - `writer.h`
  - `platform_backend.h`
  - `texture_types.h`
  - `presentation_events.h`

- Windows ANGLE D3D11 实现
  - `win_angle_platform_backend.*`
  - `win_angle_runtime.*`
  - `win_angle_texture_reader.*`
  - `win_angle_texture_writer.*`
  - `d3d11_shared_texture_slots.h`

- macOS Cocoa OpenGL + IOSurface 实现
  - `mac_cocoa_gl_platform_backend.*`
  - `mac_cocoa_gl_runtime.*`
  - `mac_cocoa_gl_texture_reader.*`
  - `mac_cocoa_gl_texture_writer.*`
  - `mac_iosurface_texture_slots.h`

- 辅助实现仍保留在 `src/framework/backend/win_angle_d3d11`
  - `qt_angle_egl_tools.*`
  - `gles2_proc_table.*`
  - `gles2_shader_utils.*`

平台层只负责 runtime、纹理导入、纹理发布，以及 backend 组装；不负责 Qt widget 或 demo 业务。

当前跨平台设计约束是：

- worker runtime 必须独立于 UI `QOpenGLContext`
- UI 与 worker 不共享 GL 对象
- 平台层负责把 worker 纹理发布到平台私有交换介质
- UI 侧始终通过 `TextureTicket` / `TextureLease` 消费结果

### `src/framework/execution`

- `execution_common.h`
  - execution 聚合兼容头

- `runtime_scope.*`
  - `IRuntime::enter()/leave()` 的 RAII 包装

- `runtime_executor.*`
  - 拥有 `IRuntime`
  - 通过 `async::SingleThreadExecutor` 顺序执行 runtime task
  - 提供 `initialize()`、`submit()`、`submitBlocking()` 与 `shutdown()`
  - 不包含 photo editor 业务类型、合并策略或结果发布逻辑

- `execution.h`
  - 聚合 `Future`、`TaskScheduler`、`SerialLane`、`LatestLane`、`MergeLane`

- `sync_lane.h`
  - 同步 lane 基础接口
  - 当前项目中尚未成为主路径

执行层只负责“在哪执行”。photo editor 主路径中，合并、丢弃、latest-only、process-again、generation 防护和结果发布都位于业务层或平台层；execution 层不持有 `IWriter`，也不理解 `photo_editor_*` 业务语义。

### `src/app`

- `texture_present_widget.*`
  - `QOpenGLWidget`
  - 通过 `IReader` 获取 `TextureTicket`
  - copy 到本地显示纹理后绘制

- `photo_editor_app_session.*`
  - 管理目录、当前图片、当前参数
  - 持有 `execution::RuntimeExecutor`
  - 持有 `PhotoEditorRuntimeService`
  - 为当前图片选择或创建 `PhotoEditorHandleActor`
  - 通过 actor 的 `setOutputSize()`、`setOpcode()`、`process()` 触发 GPU 预览
  - 不直接调用 `photo_editor_*`
  - CPU 预览是 app session 的同步单步函数，不再封装为 renderer 类
  - 初始化时把同一个 `execution::RuntimeExecutor` attach 给 `IWriter`
  - 在 GPU 结果完成后调用 `IWriter::submitTexture()`

- `photo_editor_runtime_service.*`
  - 引用 app session 持有的 `execution::RuntimeExecutor`
  - 将 `photo_editor_init` 作为独立 GL task 提交
  - 创建并管理 per-handle actor
  - shutdown 时同步销毁 actor handle，保证 runtime 关闭前完成清理

- `photo_editor_handle_actor.*`
  - 每张图一个 actor，每个 actor 管理一个 `photo_editor` handle
  - public API 是线程安全业务入口
  - 每个 GL task 最多调用一个 `photo_editor_*`
  - SDK process callback 不直接 render，而是投递独立 `photo_editor_render` task
  - 使用 generation token 屏蔽旧 task、旧 callback 和 destroy 后结果

- `async_render_main_window.*`
  - demo 主窗口

### `src/photo_editor`

- `photo_editor_result_types.h`
  - `RawGpuTextureResult`
  - `CpuImageResult`

- `photo_editor_gles2_backend.*`
  - 底层 GLES2 photo editor backend
  - 对外暴露单步 `photo_editor_*` C 风格入口

业务层不再保留 `photo_editor_render_args.h`、`PhotoEditorGpuSession`、`PhotoEditorCpuRenderer` 这类中间封装。

## 当前执行模型

当前主实现使用两类调用路径：

- GPU 异步预览
  - app 编排：`PhotoEditorAppSession`
  - 全局服务：`PhotoEditorRuntimeService`
  - handle 边界：`PhotoEditorHandleActor`
  - 执行器：`RuntimeExecutor`
  - 实际执行：actor 将 `photo_editor_init/create/set_output_size/set_opcode/process/render/destroy` 拆成单独 GL task
  - task 约束：每个提交到 execution 层的 GL task 最多调用一个 `photo_editor_*`
  - 策略归属：latest-only、process-again、destroy 优先级和 generation 防护都在 actor 内

- CPU 同步预览
  - 参数来源：当前图片、当前参数、当前输出尺寸
  - 执行方式：app worker 线程内直接同步调用 `renderCpuPreview(...)`

这意味着当前项目已经从“按 request type 注册 handler / wrapper class”进一步切换成“framework 提供 GL context 中的 `void()` 顺序执行能力，业务层自己管理 handle 状态和请求策略”。

## 当前结果模型

当前项目的结果 payload 当前收敛在 `src/photo_editor/photo_editor_result_types.h`，而不是 `framework/execution`。

当前主用两类结果：

- `RawGpuTextureResult`
  - 只表达 worker runtime 中的原始 GPU 输出
  - 还不是 UI 可读纹理

- `CpuImageResult`
  - 直接作为 CPU 图像结果交付

GPU 结果由 `PhotoEditorHandleActor::renderResult` 交回 app，app 在 `handleGpuRenderResult()` 中调用 `IWriter` 转成 `TextureTicket`，然后再交给 UI。

当前 `IWriter` 发布合同是：

- `attach(execution::RuntimeExecutor *, IWriterEvents *)`
  - 显式注入 writer 需要依赖的 runtime executor 和事件出口
- `submitTexture(GLuint sourceTextureId, QSize size, quint64 outputRevision, QString *error)`
  - 通过 runtime executor 同步进入 runtime 线程尝试发布
  - 若当前没有可立即发布的展示槽，writer 在平台层保留 latest pending frame
- `notifyPresentationCapacityAvailable()`
  - 由 reader 在释放展示槽后调用
  - writer 内部通过已 attach 的 `execution::RuntimeExecutor` 继续 drain pending publish

当前 `TextureTicket` 除 `slotIndex / generation / frameIndex / size` 外，还携带：

- `outputRevision`
  - 由 `TexturePresentWidget` 在 resize 后递增
  - app 通过 `outputSizeChanged(size, revision)` 跟随 widget 的 revision
  - writer 在发布时把 revision 写入 `TextureTicket`
  - widget 只接收当前 revision 的 ticket，用于隔离 resize 前后的结果

平台结果发布当前也不是单 pending 语义，而是：

- `D3D11SharedTextureSlots` 维护多 `Ready` 槽队列
- writer 优先获取 `Free` 槽；池满时回收最老 `Ready` 槽
- 如果当前没有可立即进入展示链路的槽，writer 会把 latest pending frame 暂存在平台层
- reader 读取 `Ready` 槽后把它转成 `Reading`
- UI 复制完成后释放为 `Free`
- reader 在释放 `Reading` 槽后，直接通知 writer 当前出现新的 publish capacity
- writer 通过已 attach 的 `execution::RuntimeExecutor` 自驱动 drain pending publish
- writer 通过 `IWriterEvents` 统一发出 `TextureTicket` 与 warning

## 当前线程与运行时边界

- UI 线程
  - `AsyncRenderMainWindow`
  - `TexturePresentWidget`

- app worker 线程
  - `PhotoEditorAppSession`

- runtime executor 线程
  - `WinAngleRuntime`
  - `MacCocoaGlRuntime`
  - 由 `execution::RuntimeExecutor` 通过 `async::SingleThreadExecutor` 管理初始化、任务执行和关闭

`PhotoEditorAppSession` 不直接触碰 `photo_editor_*` 或 GLES2 资源；这些调用被限制在 `PhotoEditorRuntimeService` / `PhotoEditorHandleActor` 提交给 `execution::RuntimeExecutor` 的 GL task 内。平台补发链路仍由 `IWriter` / 平台 backend 负责。

当前状态补充：

- Windows 主链路已验证
- macOS `framework/platform` 已按独立 context + `IOSurface` slot 方向实现
- macOS 业务可运行性仍取决于 `photo_editor_gles2_backend` 从 Windows/ANGLE 假设中继续解耦

当前 resize 相关边界是：

- widget 是 `outputRevision` 的单一真相源
- resize 时 widget 清空本地 pending ticket 队列
- worker 收到新的 `outputRevision` 后会重新提交预览请求
- writer 若之后仍发布旧 revision 的 pending frame，UI 会基于 revision 过滤掉
- UI 若收到旧 revision 的 ticket，会直接丢弃

## 当前错误模型

当前 app 层把错误分成两类：

- `initializationFailed`
  - 初始化级故障
  - UI 侧会弹错误框

- `requestWarning`
  - 普通请求失败、publish 失败或运行期警告
  - UI 侧只记录日志并短暂显示在状态栏

## 当前仓库边界

当前正式文档只有：

- [README.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/README.md)
- [ARCHITECTURE.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/ARCHITECTURE.md)

其中：

- `README.md` 负责对外概览、构建运行方式、关键文件索引
- `ARCHITECTURE.md` 负责当前框架结构、执行模型、线程边界、结果模型

以下文档不再作为当前框架的正式描述来源，只保留为历史记录或方案讨论材料：

- [docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/FRAMEWORK_RESTRUCTURE_HEADER_LAYOUT.md)
- [docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md](/D:/Desktop/AI-Agent/Gles2AsyncRender/docs/CROSS_PLATFORM_ASYNC_RENDER_FRAMEWORK_DESIGN.md)

如果历史文档与当前代码、`README.md`、`ARCHITECTURE.md` 有冲突，一律以当前代码、`README.md`、`ARCHITECTURE.md` 为准。
