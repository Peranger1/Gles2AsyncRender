# 异步框架重构方案

> 2026-05-10 状态说明
>
> 本文档描述的是旧的异步 OpenGL / 共享 `QOpenGLContext` 框架重构目标，不代表当前主分支已经落地的默认实现。
>
> 当前主分支已经切换到 D3D11 native shared texture 主路径，因此本文应视为历史方案说明，而不是当前代码结构说明。

## 目标

本文档定义异步 OpenGL 路径中，框架代码与业务代码的目标拆分方式。

目标架构中，框架层只负责以下能力：

1. 全局 GL 线程互斥。
2. 基于现有 `QOpenGLWidget` 上下文创建共享的异步上下文。
3. ANGLE/D3D11 多线程保护初始化。
4. 异步任务执行。
5. 仅对显式声明需要 OpenGL 上下文的任务包装 `makeCurrent/doneCurrent`。

框架层不应持有图片加载逻辑、图片切换逻辑、渲染参数状态、frame pool 状态，或应用层结果语义。

## 关键决策

### 1. `angle_threading.*` 属于框架层

`src/angle_threading.h` 和 `src/angle_threading.cpp` 属于框架代码。

原因：

- 它用于修正当前 GL 后端下 ANGLE/D3D11 的多线程行为。
- 它是 Windows/ANGLE 环境下让共享上下文多线程执行稳定工作的必要条件。
- 它不依赖图像处理逻辑，也不依赖任何应用特定的渲染行为。

这意味着由框架层负责：

- 显示侧上下文初始化时可以调用 `ensureAngleD3D11MultithreadProtection()`。
- worker 侧共享上下文初始化时，也可以在 `makeCurrent()` 之后调用它。
- 业务代码中不应再包含任何 ANGLE 特定探测逻辑或 D3D11 多线程保护逻辑。

该文件应与 GL 加锁机制、共享上下文创建逻辑一起，作为框架初始化路径的一部分。

### 2. Worker 输出仍然是纹理 id

worker 的输出禁止转换为 `QImage`。

原因：

- 在真实项目中，依赖上下文的异步任务天然产出的是 GPU 资源。
- worker 侧计算在应用层的结果语义，就是输出纹理 id。

这意味着本次重构不会改为 CPU readback。这里的简化来自移除纹理池协议，而不是改变结果类型。

### 3. 移除纹理池设计

`shared_texture_frame_pool.h` 应被删除。

原因：

- 当前 pool 把异步执行和帧交换协议混在了一起。
- `Free`、`Rendering`、`Pending`、`Front`、`Retiring` 这类状态，属于业务层交换语义。
- 这些内容不应该属于框架 worker。

替代方案是：由业务代码持有单个共享输出纹理。

### 4. 将业务流程简化为单张图片

应用不再需要：

- 导入图片目录。
- 下一张图片。
- 上一张图片。
- 图片列表状态。

业务流程变为：

1. 打开一张图片文件。
2. 通过 slider 调整效果参数。
3. 提交异步渲染任务。
4. 渲染到一个共享输出纹理。
5. 显示最新纹理。

## 目标分层

### 框架层

保留为框架层的文件：

- `src/gles_thread_guard.h`
- `src/gles_thread_guard.cpp`
- `src/shared_gl_environment.h`
- `src/shared_gl_environment.cpp`
- `src/shared_gl_context_handle.h`
- `src/shared_gl_context_handle.cpp`
- `src/angle_threading.h`
- `src/angle_threading.cpp`
- `src/async_gl_worker.h`（新增）
- `src/async_gl_worker.cpp`（新增）

框架层职责：

- 使用共享互斥锁保护 GL 访问。
- 基于显示上下文创建离屏共享上下文。
- 持有离屏上下文与 surface 的生命周期。
- 在需要时，对当前上下文应用 ANGLE 多线程保护。
- 在 worker 线程中异步执行任务。
- 仅在任务声明需要 GL 上下文时调用 `makeCurrent/doneCurrent`。

框架层非职责：

- 不管理图片文件状态。
- 不管理图像效果参数状态。
- 不管理纹理池或帧退休逻辑。
- 不赋予纹理 id 任何业务含义。
- 除基础设施错误外，不解释业务成功或失败。

### 业务层

保留为业务层的文件：

- `src/image_effect_types.h`
- `src/image_processing_pipeline.h`
- `src/image_processing_pipeline.cpp`
- `src/async_gles_widget.h`
- `src/async_gles_widget.cpp`
- `src/main_window.h`
- `src/main_window.cpp`
- `src/image_processing_session.h`（新增）
- `src/image_processing_session.cpp`（新增）

业务层职责：

- 加载单张图片文件。
- 保存效果参数。
- 保存输出尺寸。
- 持有唯一的共享输出纹理 id。
- 将当前图片渲染到输出纹理。
- 决定何时提交渲染任务。
- 决定纹理更新后如何显示到界面。

## 框架 Worker 契约

框架 worker 应重构为通用异步执行器。

建议的任务模型：

```cpp
class AsyncTaskContext
{
public:
    QOpenGLContext *openGlContext() const noexcept;
    bool hasOpenGlContext() const noexcept;
};

struct AsyncTask
{
    QString name;
    bool requiresOpenGlContext = false;
    std::function<void(AsyncTaskContext &)> run;
};

using AsyncTaskPtr = std::shared_ptr<AsyncTask>;
```

建议的 worker API：

```cpp
class AsyncGlWorker final : public QObject
{
    Q_OBJECT

public slots:
    bool initialize(SharedGlContextHandle *handle = nullptr);
    void enqueueTask(const AsyncTaskPtr &task);
    void shutdown();

signals:
    void infrastructureError(const QString &taskName, const QString &reason);
    void statusMessage(const QString &message);
};
```

worker 执行规则：

1. 如果 `requiresOpenGlContext == false`，则直接执行任务，绝不调用 `makeCurrent/doneCurrent`。
2. 如果 `requiresOpenGlContext == true`，则先加 GL 锁，再调用 `makeCurrent`，执行任务，随后调用 `doneCurrent`，最后解锁。
3. 如果需要 ANGLE 保护，应在相关上下文成为 current 后，作为框架初始化的一部分处理。
4. worker 不验证应用层结果是否正确。

基础设施失败包括：

- 缺少共享上下文句柄。
- `makeCurrent()` 失败。
- worker 已进入关闭状态。
- 上下文初始化失败。

业务失败仍然留在业务代码中处理。

## 业务侧渲染模型

### 单个共享输出纹理

业务代码应持有一个输出纹理：

```cpp
GLuint m_outputTextureId = 0U;
QSize m_outputTextureSize;
quint64 m_generation = 0;
```

渲染流程：

1. 确保输出纹理已创建。
2. 如有需要，调整输出纹理尺寸。
3. 在 worker 的共享上下文中渲染到该纹理。
4. 在释放 worker 上下文前调用 `glFinish()`。
5. 通知 UI 纹理内容已更新。

建议的业务信号：

```cpp
void textureUpdated(quint32 textureId, QSize size, quint64 generation);
```

它将替代以下机制：

- slot 分配
- pending frame 提升
- retiring frame 释放
- front slot 跟踪

### 重要权衡

移除纹理池后，系统将放弃生产者与消费者之间的多纹理重叠能力。

对于当前简化目标，这是可接受的，因为：

- 应用只处理单张图片，并通过 slider 做参数调节。
- 显示最新结果，比追求最大吞吐量更重要。
- 框架与业务的边界会更清晰。

代价是：

- 快速拖动 slider 时，中间结果可能被丢弃。
- 系统中只保留一个用于最新完成结果的共享输出纹理。

## 目标业务 Session

业务逻辑应迁移到独立的 session 对象中。

建议接口：

```cpp
class ImageProcessingSession
{
public:
    bool initialize(QOpenGLContext *context, QString *error);
    bool loadImageFile(QOpenGLContext *context, const QString &filePath, QString *error);
    void setOutputSize(const QSize &size);
    void setEffectParameters(const ImageEffectParameters &parameters);
    bool render(QOpenGLContext *context, QString *error);
    void release(QOpenGLContext *context);

    GLuint outputTextureId() const noexcept;
    QSize outputTextureSize() const noexcept;
    quint64 generation() const noexcept;
    QString currentImageName() const;
    bool hasImage() const noexcept;
};
```

session 职责：

- 持有 `ImageProcessingPipeline`。
- 持有一个输出纹理 id。
- 持有当前图片名称。
- 持有效果参数与输出尺寸。
- 在每次成功渲染后递增 generation 计数器。

## 简化后的 UI 模型

### 主窗口

`MainWindow` 应简化为：

- `Open Image...`
- slider 控件
- `Reset Effects`

以下内容应被移除：

- 打开图片目录
- 上一张 / 下一张
- 图片数量状态
- 当前图片索引状态
- 目录加载结果处理
- 图片选择结果处理

### 显示控件

`AsyncGlesWidget` 不应再持有 frame pool 逻辑。

它只应负责：

- 维护用于屏幕显示的 shader program
- 记录最新的共享纹理 id 与尺寸
- 在 `paintGL()` 中绘制最新纹理

以下内容应被移除：

- `SharedTextureFramePool`
- pending frame 对象
- front slot 跟踪
- retiring slot 跟踪
- 基于 `frameSwapped` 的退休逻辑

建议的 widget slot：

```cpp
void onTextureUpdated(quint32 textureId, QSize size, quint64 generation);
```

## 需要删除或迁移的现有代码

### 从业务流程中删除

以下现有行为应被移除：

- `ImageProcessingPipeline` 中的目录枚举逻辑
- 图片上一张 / 下一张切换
- frame pool 注册
- frame pool 的获取 / 提交 / 提升 / 释放

### 从当前 worker 中迁出

当前 `SharedTextureWorker` 混杂了框架与业务职责。

应保留为框架能力的部分：

- worker 线程执行
- 上下文初始化
- `makeCurrent/doneCurrent`
- GL 锁使用
- ANGLE 补丁调用

应迁出为业务能力的部分：

- 图片加载
- 输出尺寸状态
- 效果参数状态
- 渲染请求调度语义
- 输出纹理所有权
- 纹理 ready 的业务含义

## 迁移计划

### 第一阶段：建立框架 worker

1. 新增 `async_gl_worker.h/.cpp`。
2. 将通用上下文初始化逻辑从 `SharedTextureWorker` 中迁出。
3. 保留 `angle_threading.*` 作为框架依赖，供显示侧与 worker 启动路径使用。

### 第二阶段：简化业务 pipeline

1. 将基于目录的图片加载改为单文件加载。
2. 删除上一张 / 下一张图片逻辑。
3. 在 `ImageProcessingPipeline` 和 `ImageProcessingSession` 中只保留单图状态。

### 第三阶段：移除 frame pool

1. 删除 `shared_texture_frame_pool.h`。
2. 从 widget 与 worker 路径中移除 pool 的使用。
3. 改为由业务 session 持有单个共享输出纹理。

### 第四阶段：重新连接 UI

1. 将 `Open Image Directory` 替换为 `Open Image...`。
2. 删除上一张 / 下一张相关操作。
3. 保留 slider 与渲染触发行为。
4. 更新状态栏，显示当前文件名、输出尺寸和渲染耗时。

## 设计约束

重构后必须满足以下约束：

1. 框架代码除了通用 Qt/OpenGL 类型外，不引入图片特定的业务类型。
2. 任务执行结果是否正确，不属于框架责任。
3. 不需要上下文的 GL 任务，绝不能触发 `makeCurrent/doneCurrent`。
4. `angle_threading.*` 必须继续作为框架初始化路径的一部分，并且只在上下文已经 current 时应用。
5. 输出结果保持为纹理 id，而不是 `QImage`。
6. worker 和显示控件中都不再保留纹理池状态机。

## 最终状态

完成重构后：

- 框架成为可复用的异步 OpenGL 执行层
- `angle_threading.*` 被明确纳入该框架
- 业务代码被收敛为单张图片加 slider 的编辑流程
- worker 输出仍然是纹理 id
- frame pool 逻辑被移除
- 架构能够清晰地区分基础设施与应用行为
