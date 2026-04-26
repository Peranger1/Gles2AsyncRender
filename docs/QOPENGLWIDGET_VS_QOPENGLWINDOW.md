# QOpenGLWidget 与 QOpenGLWindow 对比分析

本文整理 Qt 5 官方文档中关于 `QOpenGLWidget` 与 `QOpenGLWindow` 的定位、更新语义、性能取舍，以及 `QOpenGLWindow + QWidget::createWindowContainer()` 方案的适用边界。

本文只参考 Qt 官方 Qt 5 归档文档，交叉核对了 Qt 5.12 与 Qt 5.15 页面，不使用 Qt 6 文档。

## 1. 结论先行

如果目标是稳定地嵌入传统 `QWidget` 界面，并与按钮、面板、布局、滚动区域等常规 UI 正常协作，优先使用 `QOpenGLWidget`。

如果目标是一个独立 OpenGL 窗口，或者你明确需要减少一次额外的 widget 合成步骤，并且界面结构简单、不依赖复杂叠放和透明混排，可以考虑 `QOpenGLWindow`。

如果希望在 `QWidget` 界面里为了性能使用 `QOpenGLWindow`，只能通过 `QWidget::createWindowContainer()` 嵌入。但 Qt 官方对这种方式明确给出了多项限制，因此它不能视为 `QOpenGLWidget` 的通用高性能替代品。

## 2. 基本定位区别

### 2.1 QOpenGLWidget

- 继承自 `QWidget`
- 属于 `widgets` 模块
- 天然适合放入 `QLayout`
- 目标是把 OpenGL 视图稳定地整合进现有 widget 体系

Qt 官方将其定义为：

- 面向 Qt Widgets 应用的 OpenGL 渲染控件
- `QGLWidget` 的现代替代方案
- 稳定的跨平台方案

### 2.2 QOpenGLWindow

- 基于 `QWindow` / `QPaintDeviceWindow`
- 属于 `gui` 模块
- 默认是独立窗口，不是 `QWidget`
- 若要嵌入 `QWidget` 界面，必须借助 `QWidget::createWindowContainer()`

Qt 官方对它的定位是：

- 与 `QOpenGLWidget` API 风格相近
- 不依赖 `widgets` 模块
- 默认路径下性能更好

这里“性能更好”的前提，是它在默认模式下没有 `QOpenGLWidget` 那一步额外的 widget 纹理合成。

## 3. 渲染模型区别

### 3.1 QOpenGLWidget 的渲染模型

`QOpenGLWidget` 始终先渲染到离屏 FBO。

其大致流程是：

1. `paintGL()` 把内容画进 widget 自己的 FBO
2. 顶层窗口随后把这张 FBO 纹理与其他普通 `QWidget` 一起合成
3. 最终顶层窗口再完成一次 `swapBuffers()`

因此：

- `QOpenGLWidget` 不是直接画到窗口默认 framebuffer
- 它天然参与整个 widget 窗口的合成流程
- 一旦某个顶层窗口中放入 `QOpenGLWidget`，Qt 官方文档明确说明该顶层窗口会开启基于 OpenGL 的 compositing

这种方式的优势是：

- 更容易与普通 `QWidget` 正常混排
- 跨平台行为更稳定
- 更适合复杂桌面 UI

代价是：

- 多了一次离屏渲染和合成步骤
- 对大视口连续重绘来说，路径不如直接窗口渲染那么直接

### 3.2 QOpenGLWindow 的渲染模型

`QOpenGLWindow` 默认直接渲染到窗口表面。

在默认更新模式下：

- 没有额外的持久化 FBO
- `paintGL()` 直接面向窗口默认 framebuffer
- 每次重绘后由 Qt 内部执行 `swapBuffers()`

因此：

- 路径更接近传统 OpenGL 窗口
- 少了一次 widget 合成步骤
- 在整帧重绘场景下通常更直接

这也是 Qt 官方说它默认性能更好的原因。

## 4. 更新语义分析

“更新语义”主要回答三个问题：

- 调用 `update()` 后会发生什么
- 每一帧画到哪里
- 下一帧能否依赖上一帧留下的内容

### 4.1 共同点

`QOpenGLWidget` 和 `QOpenGLWindow` 都不是在调用 `update()` 的瞬间立即执行 `paintGL()`。

`update()` 的语义都是：

- 请求一次异步重绘
- Qt 后续在合适时机触发真正的绘制事件

因此不要把 `update()` 理解为“立即绘制”。

### 4.2 QOpenGLWidget 的更新语义

`QOpenGLWidget` 提供两种更新模式：

- `NoPartialUpdate`
- `PartialUpdate`

#### NoPartialUpdate

该模式表示：

- 每帧绘制完成后，颜色缓冲和相关附属缓冲的内容会被视为无效
- 下一次 `paintGL()` 不应依赖上一帧保留内容
- 典型做法是每次完整清屏并重绘整个视图

Qt 官方还特别说明：

- 从 Qt 5.5 开始，`QOpenGLWidget` 的默认行为是 `NoPartialUpdate`
- 在 Qt 5.5 之前，默认行为是保留前一帧内容

这意味着在 Qt 5 系列里，如果需要兼容到 5.4.x，需要特别注意默认行为差异。

#### PartialUpdate

该模式表示：

- widget 内部 FBO 的内容在帧之间保留
- 下一次 `paintGL()` 可以基于上一帧做增量绘制

这种模式适合：

- 画布类应用
- 逐步累积绘制
- 使用 `QPainter` 做局部更新的场景

但不适合把它误解为“白赚性能”。只有在确实可以避免整帧重绘时，保留前一帧内容才有意义。

#### 对工程实现的含义

如果你的渲染逻辑本来就是：

- 每帧 `glClear()`
- 整个场景完整重画

那么 `PartialUpdate` 通常没有实际收益。

如果你的渲染逻辑是：

- 每次只改动局部区域
- 或者依赖前一帧内容叠加绘制

那么 `PartialUpdate` 才值得考虑。

### 4.3 QOpenGLWindow 的更新语义

`QOpenGLWindow` 提供三种更新模式：

- `NoPartialUpdate`
- `PartialUpdateBlit`
- `PartialUpdateBlend`

#### NoPartialUpdate

该模式是默认模式，语义最直接：

- 每次更新都重画整个窗口表面
- 没有额外 framebuffers
- 行为等价于普通 OpenGL `QWindow`

这也是 `QOpenGLWindow` 最有性能优势的模式。

#### PartialUpdateBlit

该模式表示：

- Qt 在内部创建一个额外 FBO
- `paintGL()` 先画到该 FBO
- 每次绘制后再把这个 FBO blit 到窗口默认 framebuffer

它的意义是：

- 保留上一帧内容
- 允许增量绘制
- 避免每次都把整个窗口完整重画

它的代价是：

- 不再是最直接的窗口绘制路径
- 引入额外 FBO 和一次 blit

#### PartialUpdateBlend

该模式与 `PartialUpdateBlit` 类似，但最终不是用 framebuffer blit，而是：

- 把额外 FBO 当作纹理
- 通过绘制一个带混合的 textured quad 合成到窗口上

它的意义是：

- 支持 alpha blended 内容
- 在某些不支持 `glBlitFramebuffer` 的环境也可工作

Qt 官方明确指出：

- 该模式通常会比 `PartialUpdateBlit` 更慢

#### 对工程实现的含义

`QOpenGLWindow` 真正的“性能路径”是：

- `NoPartialUpdate`
- 每帧完整重绘

一旦你切换到 `PartialUpdateBlit` 或 `PartialUpdateBlend`，本质上就是重新引入：

- 额外 FBO
- 内容保留
- 复制或混合步骤

此时它和 `QOpenGLWidget` 的差异会缩小，只是渲染管线细节不同。

### 4.4 frameSwapped 语义差异

这两个类都提供 `frameSwapped()`，但触发时机的语义并不完全一样。

对于 `QOpenGLWindow`：

- 它是在窗口完成 buffer swap 之后发出
- 更接近“这次窗口显示已经真正提交到交换链”

对于 `QOpenGLWidget`：

- 它是在顶层窗口完成对各个 widget 的合成，并从顶层窗口的 `swapBuffers()` 返回之后发出
- 更接近“顶层窗口完成这次 composition”

因此：

- `QOpenGLWindow` 的 `frameSwapped()` 更像直接窗口渲染节拍
- `QOpenGLWidget` 的 `frameSwapped()` 属于整个顶层窗口的合成节拍

## 5. 性能理解方式

### 5.1 为什么 QOpenGLWindow 默认更快

Qt 官方说 `QOpenGLWindow` 更快，原因很明确：

- 没有 `QOpenGLWidget` 那一步把离屏结果再与普通 widget 合成的额外步骤

因此如果场景是：

- 一个主 OpenGL 视口
- 界面结构简单
- 每帧整屏重绘

那么 `QOpenGLWindow` 的路径通常更直接。

### 5.2 为什么不能机械地把它理解成“总是更快”

一旦 `QOpenGLWindow` 为了嵌入 widget 界面而改成：

- `createWindowContainer()`
- 或切到 partial update 模式

你就重新引入了其他复杂成本：

- native child window 的平台限制
- 额外 FBO
- 焦点与叠放问题
- 滚动区与 MDI 的额外开销

所以它不是 `QOpenGLWidget` 的无脑高性能替代品。

## 6. createWindowContainer() 的本质

`QOpenGLWindow` 若想嵌入 `QWidget` 界面，必须通过：

- `QWidget::createWindowContainer()`

Qt 官方对这个容器的定义是：

- 创建一个 `QWidget`
- 用来把一个 `QWindow` 嵌进 `QWidget` 应用

但这个“容器”并不会把 `QWindow` 真正变成普通 `QWidget`。

它的本质是：

- 顶层 widget 窗口下挂了一个 native child window

这一点非常关键，因为后面的所有限制都来自这个事实。

## 7. QOpenGLWindow + createWindowContainer() 的场景限制

Qt 官方在 `QWidget::createWindowContainer()` 文档中明确列出了限制；`QOpenGLWidget` 文档也把这条路径描述为：

- 只有在别无选择时才应使用
- 不适合多数嵌入式和移动平台
- 在某些桌面平台上有已知问题，例如 macOS
- 稳定的跨平台方案始终是 `QOpenGLWidget`

下面按实际场景整理这些限制。

### 7.1 重叠与叠放限制

Qt 官方明确写到：

- 嵌入窗口会作为一个不透明盒子叠放在 widget 层级之上
- 多个 window container 相互重叠时，叠放顺序未定义

这意味着：

- 普通 `QWidget` 想稳定盖在它上面，不可靠
- 两个 container 彼此部分重叠，不可靠
- 各种浮动 overlay、rubber band、提示层等效果，都不应假定能正常工作

这也是 Qt 官方在 `QOpenGLWidget` 文档里特别提到 overlap 问题的原因。

### 7.2 透明与半透明限制

由于它是 native child window，而不是参与统一 widget 合成的一块纹理，因此透明混排会很受限。

具体表现通常是：

- 想让下面普通 widget 透出来，不适合
- 想在它上面稳定叠一个半透明 widget，不适合
- 多个 container 之间做 alpha 叠加，更不应依赖

因此 Qt 官方把 transparency 也明确列为该方案的限制场景之一。

### 7.3 Scroll View 限制

Qt 官方文档点名：

- 若 container 作为 `QAbstractScrollArea` 子对象使用
- Qt 会为其父链上的每一层 widget 创建 native window
- 这样做是为了保证 stacking 和 clipping

同时官方也警告：

- 有很多 native child windows 时，应用整体性能可能受影响

因此在滚动区域中使用该方案的问题不只是“可能显示怪”，而是：

- 父链 native 化
- 系统窗口数量上升
- 裁剪与滚动成本提高
- 多实例时性能会明显变差

### 7.4 MDI 场景限制

Qt 官方同样点名：

- `QMdiArea` 是受限制场景之一

原因与滚动区相同：

- 需要额外的 native window 链保证 stacking/clipping
- 多个子窗口中若都嵌 container，复杂度会快速升高

因此在 MDI 界面里：

- 焦点
- 覆盖关系
- 窗口切换
- 多实例性能

都会比普通 `QWidget` / `QOpenGLWidget` 路径复杂得多。

### 7.5 焦点处理限制

Qt 官方明确说明：

- container 可以把焦点委托给内部 `QWindow`
- 但如何从该 `QWindow` 回到普通 widget 的焦点链，要靠 `QWindow` 自身实现
- `QWindow::requestActivate()` 能否真正激活焦点，还与平台有关

这意味着：

- 进入该区域的焦点切换相对可控
- 从该区域返回外部 widget 的 `Tab` 焦点链，可能需要额外处理
- 某些平台上行为不完全一致

如果外层界面是传统表单、复杂快捷键、严格 tab 顺序控制，那么这一点必须提前评估。

### 7.6 渲染集成限制

Qt 官方明确说明该方案不与以下能力协同：

- `QGraphicsProxyWidget`
- `QWidget::render()`
- 以及类似的渲染重定向功能

这意味着：

- 不能指望整个 widget 树截图或离屏导出时自动包含该内容
- 不能把它当成一个完全服从 widget 渲染体系的普通控件

### 7.7 多实例性能限制

Qt 官方直说：

- 在 `QWidget` 应用中使用很多个 window container，会显著伤害整体性能

因此：

- 它更适合少量、大块、关键视口
- 不适合一个界面中铺很多个嵌入式 OpenGL 子视图

## 8. macOS 相关结论

Qt 官方在 `QOpenGLWidget` 文档中对 `QOpenGLWindow + createWindowContainer()` 的评价里，明确提到：

- 某些桌面平台存在已知问题
- macOS 在其例子中被点名

因此对 macOS 应保持保守结论：

- `QOpenGLWindow + createWindowContainer()` 不是推荐优先路线
- 若目标是稳定的跨平台 widget 嵌入显示，应优先选择 `QOpenGLWidget`

另外，Qt 官方还特别提醒：

- 在某些平台，例如 macOS，如果请求 OpenGL core profile
- `QSurfaceFormat::setDefaultFormat()` 需要在构造 `QApplication` 之前调用

这条规则主要影响的是：

- 上下文共享
- 内部上下文创建一致性

对使用 `QOpenGLWidget` 的多上下文场景尤其重要。

## 9. 典型选型建议

### 9.1 优先使用 QOpenGLWidget 的情况

- 要放入常规 `QWidget` 布局
- 需要与按钮、面板、工具条稳定混排
- 需要较好的跨平台一致性
- 需要避免 native child window 带来的叠放和焦点问题
- 目标平台包含 macOS
- 界面里可能有滚动区、MDI、复杂 overlay 或多个 OpenGL 视图

### 9.2 可以考虑 QOpenGLWindow 的情况

- 需要独立 OpenGL 窗口
- 界面结构简单
- 每帧整屏重绘
- 追求尽量直接的显示路径
- 不需要把它深度嵌入复杂 widget 层级

### 9.3 可以谨慎考虑 createWindowContainer() 的情况

- 只有少量嵌入实例
- 没有复杂重叠和透明混排需求
- 不在 scroll area / MDI 中使用
- 焦点处理可接受定制
- 平台行为已做过专项验证

它更适合：

- “单个主视口 + 少量简单外围控件” 的桌面界面

而不适合：

- “复杂 widget 桌面应用的通用 OpenGL 承载方式”

## 10. 对当前项目的直接启示

对本项目这类以 Qt Widgets 为主、并需要跨平台扩展的工程来说，默认建议仍应是：

- UI 线程使用 `QOpenGLWidget` 做显示
- worker 线程使用共享 `QOpenGLContext` + `QOffscreenSurface`
- 通过共享纹理在 worker 与 UI 之间传递 GPU 结果

如果未来某个特定页面因为单视口性能原因想尝试 `QOpenGLWindow + createWindowContainer()`，应把它当成：

- 针对特定场景的专项优化

而不是整体架构默认方案。

## 参考文档

- Qt 5.15 `QOpenGLWidget`
- Qt 5.15 `QOpenGLWindow`
- Qt 5.15 `QWidget::createWindowContainer()`
- Qt 5.12 `QOpenGLWidget`
- Qt 5.12 `QOpenGLWindow`
- Qt 5.12 `QWidget::createWindowContainer()`

对应链接：

- https://doc.qt.io/archives/qt-5.15/qopenglwidget.html
- https://doc.qt.io/archives/qt-5.15/qopenglwindow.html
- https://doc.qt.io/archives/qt-5.15/qwidget.html#createWindowContainer
- https://doc.qt.io/archives/qt-5.12/qopenglwidget.html
- https://doc.qt.io/archives/qt-5.12/qopenglwindow.html
- https://doc.qt.io/archives/qt-5.12/qwidget.html#createWindowContainer
