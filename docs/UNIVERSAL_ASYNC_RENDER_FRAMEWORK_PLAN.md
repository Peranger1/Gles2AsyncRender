# 通用异步渲染框架落地方案

> 2026-05-11
>
> 本文档定义当前工程下一阶段的目标：在现有 `Windows + Qt 5.15.1 + ANGLE + GLES2 + D3D11 shared texture` 主路径基础上，进一步抽象出一套与具体算法业务无关、与具体显示组件无关的通用异步渲染框架。
>
> 本文档不是重新发明一套新架构，而是基于当前已经验证通过的实现继续收敛。

## 1. 目标

当前主分支已经证明下面这条链路是成立的：

- worker 侧使用独立 standalone ANGLE runtime
- worker 侧输出结果先发布到 D3D11 shared texture slot
- UI 侧通过 Qt 当前持有的 ANGLE/EGL 运行时导入 shared texture
- UI 侧在 `paintGL()` 中执行 `copy-on-acquire`
- shared slot 生命周期已经收敛到 `Free -> Rendering -> Pending -> Free`

下一阶段的目标不是再去验证这条链路本身，而是把它整理成清晰的四层结构：

1. 平台互操作层
2. 异步执行层
3. 显示接入层
4. 业务适配层

最终结果应满足：

- 框架层不依赖图片编辑业务
- 框架层不依赖 `QOpenGLWidget`
- 业务层不直接管理 slot pool
- 显示层不直接接触算法 session
- 只要显示组件支持 GLES2.0，并能提供本运行时需要的上下文环境，就可以复用这套框架

## 2. 当前实现与目标拆分

当前代码中已经接近框架层的模块包括：

- [src/framework/backend/win_angle_d3d11/angle_standalone_runtime.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/backend/win_angle_d3d11/angle_standalone_runtime.h:1)
- [src/framework/backend/win_angle_d3d11/gles2_proc_table.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/backend/win_angle_d3d11/gles2_proc_table.h:1)
- [src/framework/backend/win_angle_d3d11/d3d11_frame_publisher.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/backend/win_angle_d3d11/d3d11_frame_publisher.h:1)
- [src/framework/backend/win_angle_d3d11/d3d11_shared_slot_pool.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/backend/win_angle_d3d11/d3d11_shared_slot_pool.h:1)
- [src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.h:1)

当前仍然混合了框架职责和业务职责的模块包括：

- [src/d3d11_native_worker.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/d3d11_native_worker.h:1)
- [src/d3d11_native_worker.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/d3d11_native_worker.cpp:1)
- [src/framework/qt/qopenglwidget_frame_view.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/qt/qopenglwidget_frame_view.h:1)
- [src/framework/qt/qopenglwidget_frame_view.cpp](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/framework/qt/qopenglwidget_frame_view.cpp:1)
- [src/photo_editor_session.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor_session.h:1)
- [src/photo_editor_library_host.h](/D:/Desktop/AI-Agent/Gles2AsyncRender/src/photo_editor_library_host.h:1)

当前混杂点主要有三类：

- worker 同时负责线程调度、请求合并、session 生命周期、图片目录状态、渲染、publish
- import widget 同时负责 `QOpenGLWidget` 壳、Qt ANGLE 探测、shared texture import、local display copy、显示逻辑
- 业务 session 仍然通过 worker 的外层流程隐式控制，没有形成清晰的算法适配接口

## 3. 目标分层

### 3.1 平台互操作层

这一层负责“如何把一个 worker 侧 GLES2 结果安全交给显示侧运行时”。

这一层应包含：

- 独立上下文创建
- EGL / GLES2 函数解析
- standalone runtime 生命周期
- D3D11 shared texture 资源创建
- slot pool 元数据
- publish 到 shared slot
- display 侧 import / copy / release

这一层不应包含：

- 图片加载
- 滤镜参数
- UI 交互
- 请求节流策略
- 业务语义上的成功 / 失败解释

### 3.2 异步执行层

这一层负责“如何在 worker 线程上组织异步请求并产出一帧”。

这一层应包含：

- worker 线程执行器
- render request 入队
- latest-wins 请求收敛
- request 合并
- render in flight 状态
- 等待 free slot
- frame published 事件
- shutdown / flush / restart 生命周期

这一层不应包含：

- 具体算法类型
- 具体显示控件类型
- 图片目录或图片索引状态

### 3.3 显示接入层

这一层负责“如何把 pending frame 接到某个具体显示组件上”。

这一层应包含：

- 从显示组件获取当前 GLES2 环境
- 解析当前 Qt / ANGLE 的 EGL 入口
- 导入 shared texture
- 复制到显示组件自己的 local display texture
- 在显示组件的绘制时机消费最新帧

这一层不应包含：

- 算法业务逻辑
- worker 任务调度
- slot 分配策略

### 3.4 业务适配层

这一层负责“如何在 worker runtime 上执行某个具体算法，并产出 output texture”。

这一层应包含：

- 算法库一次性初始化
- 单个算法 session 生命周期
- 输入资源准备
- 参数应用
- process / render 调用
- 输出 `textureId + size`

这一层不应包含：

- shared texture 交接
- import widget 逻辑
- 显示控件逻辑

## 4. 建议目录结构

建议在当前工程内逐步收敛到如下目录结构：

```text
src/
  framework/
    core/
      async_render_types.h
      async_render_executor.h/.cpp
      async_render_session.h
      frame_publisher.h
      display_presenter.h
    backend/
      win_angle_d3d11/
        angle_standalone_runtime.h/.cpp
        gles2_proc_table.h/.cpp
        gles2_shader_utils.h/.cpp
        d3d11_shared_slot_pool.h/.cpp
        d3d11_frame_publisher.h/.cpp
        qt_angle_display_importer.h/.cpp
        qt_angle_egl_tools.h/.cpp
    qt/
      qt_gl_display_host.h
      qopenglwidget_display_host.h/.cpp
      qopenglwidget_frame_presenter.h/.cpp
  adapters/
    photo_editor/
      photo_editor_render_session.h/.cpp
      photo_editor_library_host.h/.cpp
  app/
    async_render_main_window.h/.cpp
```

第一阶段不要求一次性把所有文件移动到上述目录，但新增代码应优先按这个结构放置。

## 5. 框架核心对象

### 5.1 渲染请求与结果

建议新增统一类型：

```cpp
struct AsyncRenderRequest
{
    quint64 sequence = 0;
    QSize outputSize;
    std::shared_ptr<void> payload;
};

struct RenderedTexture
{
    GLuint textureId = 0;
    QSize size;
};

struct PublishedFrame
{
    int slotIndex = -1;
    quintptr sharedHandle = 0;
    QSize size;
    quint64 generation = 0;
    quint64 frameIndex = 0;
};
```

说明：

- `AsyncRenderRequest` 是异步执行层唯一认识的输入
- `payload` 由业务层自解释，框架不解析
- `RenderedTexture` 是业务 session 产出的 worker-side 结果
- `PublishedFrame` 是平台互操作层产出的跨线程交接结果

### 5.2 运行时接口

建议把 runtime 契约显式抽象出来：

```cpp
class IRenderRuntime
{
public:
    virtual ~IRenderRuntime() = default;

    virtual bool initialize(QString *error) = 0;
    virtual bool makeCurrent(QString *error) = 0;
    virtual bool doneCurrent(QString *error) = 0;
    virtual void shutdown() = 0;

    virtual void *resolveProc(const char *name) const = 0;
    virtual const Gles2ProcTable &procTable() const = 0;
};
```

Windows 首个实现就是把现有 `AngleStandaloneRuntime` 整理为 `IRenderRuntime` 的实现。

### 5.3 业务 session 接口

建议把具体算法适配为统一接口：

```cpp
class IAsyncRenderSession
{
public:
    virtual ~IAsyncRenderSession() = default;

    virtual bool initialize(IRenderRuntime &runtime, QString *error) = 0;
    virtual bool render(const AsyncRenderRequest &request,
                        RenderedTexture *output,
                        QString *error) = 0;
    virtual void shutdown() = 0;
};
```

约束如下：

- `render()` 调用期间由执行层保证 runtime 已经 current
- session 只负责从 request 产出 `RenderedTexture`
- session 不分配 shared slot
- session 不直接发 UI 信号

### 5.4 发布接口

建议把 shared texture publish 收敛为统一契约：

```cpp
class IFramePublisher
{
public:
    virtual ~IFramePublisher() = default;

    virtual bool initialize(IRenderRuntime &runtime, QString *error) = 0;
    virtual bool publish(const RenderedTexture &source,
                         PublishedFrame *frame,
                         QString *error) = 0;
    virtual void shutdown() = 0;
};
```

Windows 首个实现应直接由当前 `D3D11FramePublisher` 演化而来。

### 5.5 显示接入接口

建议把显示接入侧拆成两层：

```cpp
class IDisplayHost
{
public:
    virtual ~IDisplayHost() = default;

    virtual QOpenGLContext *glContext() const = 0;
    virtual QSize outputPixelSize() const = 0;
    virtual void requestUpdate() = 0;
};

class IFramePresenter
{
public:
    virtual ~IFramePresenter() = default;

    virtual bool initialize(IDisplayHost &host, QString *error) = 0;
    virtual bool consume(const PublishedFrame &frame, QString *error) = 0;
    virtual void paint() = 0;
    virtual void shutdown() = 0;
};
```

说明：

- `IDisplayHost` 代表具体显示组件壳
- `IFramePresenter` 代表互操作和显示逻辑
- `QOpenGLWidget` 只是一个 `IDisplayHost` 实现

## 6. 异步执行模型

### 6.1 核心原则

建议框架层固定采用“单 worker 串行执行 + latest wins”模型。

原因：

- 当前业务场景本质上只需要最新结果
- 多个并行 worker 会让 runtime、slot、session 生命周期复杂化
- 当前 shared slot 设计也更适合单生产者

### 6.2 推荐状态机

执行层建议维护如下状态：

```cpp
enum class ExecutorState
{
    Idle,
    Processing,
    Publishing,
    WaitingForSlot,
    Stopping,
    Failed
};
```

### 6.3 推荐规则

规则如下：

1. 同一时刻只允许一个 `render()` 执行。
2. 如果新请求到达而旧请求尚未开始执行，旧请求直接被覆盖。
3. 如果新请求到达时当前处于 `Processing`，只记录最新请求，当前轮完成后再执行下一轮。
4. 如果算法已经完成但没有 free slot，则进入 `WaitingForSlot`。
5. UI 在 slot 释放后只唤醒执行层，不重新解释业务状态。
6. `shutdown()` 必须让执行层停止再进入新一轮 request。

### 6.4 为什么这里不做多队列

当前阶段不建议引入：

- 多优先级队列
- 多 worker runtime
- request 回放
- 历史帧缓存

这些能力会放大同步复杂度，但并不直接提升当前场景的核心收益。

## 7. 平台互操作层的收敛方式

### 7.1 当前可直接保留的能力

现有实现中，下面几部分已经接近目标：

- `AngleStandaloneRuntime`
  - 独立 D3D11 device
  - 独立 EGLDisplay / EGLContext / pbuffer
  - 统一 proc table
- `D3D11FramePublisher`
  - shared texture slot 资源
  - GPU publish + CPU fallback
- `D3D11SharedSlotPool`
  - `Free / Rendering / Pending`
- `QOpenGLWidgetFrameView::copyFrameToDisplayTexture()`
  - keyed mutex acquire/release
  - `eglBindTexImage`
  - local display copy

### 7.2 需要改名和收敛的点

建议逐步进行如下调整：

- `D3D11SharedSlotPool`
  - 已完成从 `D3D11NativeSlotPool` 的命名收敛
  - 明确它是框架 backend 层对象，不再带 `NativeWorker` 语义
- `D3D11FramePublisher`
  - 已完成从 `D3D11StandalonePublishBridge` 的命名收敛
  - 类名直接体现“frame publisher”而不是“bridge”
- `QOpenGLWidgetFrameView` 中的 import/copy 逻辑
  - 下沉为 `QtAngleFramePresenter`
  - widget 本身只保留外壳职责

### 7.3 这一层的稳定边界

这一层最终对上层暴露的应该只有：

- runtime 初始化
- slot acquire / submit / release
- publish output texture
- consume published frame

上层不应再知道：

- `EGLSurface`
- `IDXGIKeyedMutex`
- `eglBindTexImage`
- `AcquireSync`

## 8. 显示接入层的收敛方式

### 8.1 当前问题

现在的 `QOpenGLWidgetFrameView` 同时承担了：

- `QOpenGLWidget` 生命周期
- Qt ANGLE/EGL 运行时探测
- pending frame 缓存
- shared texture import
- local display texture copy
- final paint

这会导致框架天然绑死在 `QOpenGLWidget` 上。

### 8.2 目标拆分

建议拆成三个对象：

1. `QOpenGLWidgetDisplayHost`
   - 只负责 widget 封装、尺寸变化、update、paint hook
2. `QtAngleDisplayEnvironment`
   - 负责从当前 Qt context 解析 EGL 入口和 display identity
3. `QtAngleFramePresenter`
   - 负责 consume pending frame、copy 到 local texture、最终绘制

### 8.3 对显示组件的最小要求

框架不要求显示组件必须是 `QOpenGLWidget`，但要求它满足：

- 存在可用的 GLES2 上下文
- 允许在绘制时机调用 GLES2 API
- 能提供像素输出尺寸
- 能接受“有新帧请重绘”的通知

因此理论上可以适配：

- `QOpenGLWidget`
- `QOpenGLWindow`
- `QQuickFramebufferObject`
- 其他具备相同能力的宿主

## 9. 业务适配层的收敛方式

### 9.1 当前问题

现在的 `D3D11NativeWorker` 直接持有：

- 图片目录
- 当前图片索引
- 当前 `QImage`
- 参数脏标记
- `PhotoEditorSession`
- publish 过程控制

这意味着 worker 本身已经不是通用执行器，而是“图片编辑应用 worker”。

### 9.2 目标形态

建议新增：

- `PhotoEditorRenderSession`

职责：

- `photo_editor_init`
- 当前图片输入准备
- 参数快照应用
- `photo_editor_process`
- `photo_editor_render`
- 输出 `RenderedTexture`

执行器不再直接持有：

- 图片目录
- 当前图片索引
- `QImage`
- 业务参数结构

这些都应先收敛到业务层 request 中。

### 9.3 第一阶段不必过度泛化

即便在引入 `IAsyncRenderSession` 后，也不要求马上做到：

- 任意 payload 自动反序列化
- 任意算法库插件动态装载
- 多业务类型统一 schema

第一阶段只要做到：

- worker 不再知道 Photo Editor 业务细节
- Photo Editor 通过自己的 session adapter 接入框架

就已经足够。

## 10. 推荐迁移步骤

### 阶段一：冻结当前主链路，抽出接口

目标：

- 不改变现有可运行行为
- 先把接口边界写清楚

步骤：

1. 新增 `framework/core/async_render_types.h`
2. 新增 `IRenderRuntime`、`IAsyncRenderSession`、`IFramePublisher`、`IDisplayHost`、`IFramePresenter`
3. 让现有 `AngleStandaloneRuntime`、`D3D11FramePublisher` 在不改行为的前提下适配这些接口

阶段完成标准：

- 当前 demo 行为不变
- 新接口可以覆盖当前主链路

### 阶段二：拆 worker

目标：

- 把业务 session 从执行器里剥离出来

步骤：

1. 新增 `AsyncRenderExecutor`
2. 把 `D3D11NativeWorker` 中的调度逻辑迁到 `AsyncRenderExecutor`
3. 新增 `PhotoEditorRenderSession`
4. 把当前 `D3D11NativeWorker` 中的图片编辑流程迁到 `PhotoEditorRenderSession`

阶段完成标准：

- 执行器不再知道图片目录、图片索引、业务参数结构
- session 自己产出 `RenderedTexture`

### 阶段三：拆 presenter

目标：

- 把 `QOpenGLWidgetFrameView` 从“框架 + 控件”改为“控件壳 + presenter”

步骤：

1. 新增 `QOpenGLWidgetDisplayHost`
2. 新增 `QtAngleFramePresenter`
3. 把 `copyFrameToDisplayTexture()`、imported slot 管理、display texture 绘制迁入 presenter
4. `QOpenGLWidgetFrameView` 只保留 widget 壳和信号转发

阶段完成标准：

- presenter 可以脱离当前 widget 文件独立存在
- widget 文件中不再直接出现大段 interop 细节

### 阶段四：清理命名与目录

目标：

- 让代码结构与设计一致

步骤：

1. `D3D11NativeSlotPool` 更名为 `D3D11SharedSlotPool`
2. `D3D11StandalonePublishBridge` 更名为 `D3D11FramePublisher`
3. `D3D11ImportWidget` 调整为 `QOpenGLWidgetFrameView` 或等价名称
4. 把 backend 相关文件移动到 `framework/backend/win_angle_d3d11/`

阶段完成标准：

- 文件命名不再带过强的 demo 语义
- 目录结构能直接反映分层

## 11. 风险与约束

### 11.1 不要过早追求跨平台实现

当前最正确的做法是：

- 先定义平台无关接口
- 先只实现 Windows ANGLE/D3D11 后端

不要在第一阶段就试图同时写出：

- macOS CGL backend
- Linux EGL backend
- Linux GLX backend

那会让当前重构被平台细节拖垮。

### 11.2 不要回退到共享 Qt `QOpenGLContext`

当前已经验证可行的边界是：

- UI runtime 独立
- worker runtime 独立
- 通过 shared texture slot 交接

因此不要为了“抽象统一”反而重新把 worker 拉回共享 `QOpenGLContext` 主路径。

### 11.3 不要把 slot pool 提升为业务协议

slot pool 是框架 backend 层的资源复用协议，不应继续向上层泄漏为业务层状态机。

业务层只应该看到：

- request
- output texture
- published frame ready

### 11.4 不要让 presenter 直接调用业务接口

presenter 只负责消费 frame，不应反向控制：

- 算法参数
- 图片切换
- 业务 session 生命周期

否则显示层和业务层会再次耦合。

## 12. 最终目标形态

完成上述重构后，这个工程应收敛为下面的稳定结构：

- backend 层负责独立 runtime、EGL/GLES2 入口、shared texture、slot pool、publish/import
- executor 层负责异步执行模型和请求收敛
- presenter 层负责把 pending frame 呈现到具体显示组件
- adapter 层负责把具体算法库包装成统一 session
- app 层只负责把 UI 事件转成 request，把 frame ready 接到 presenter

用一句话概括，最终框架应稳定在：

`session 产出 worker-side texture，publisher 负责跨 runtime 交接，presenter 负责显示，executor 负责把这一切串起来。`

这就是当前工程继续演进时最稳妥、也最符合现有代码事实的收敛方向。
