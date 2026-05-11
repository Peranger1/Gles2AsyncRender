#include "d3d11_import_widget.h"

#include "src/angle_threading.h"
#include "src/framework/qt/qt_angle_display_presenter.h"
#include "src/runtime_diagnostics.h"

namespace
{
void logImportWidgetMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[D3D11ImportWidget]", message);
}

void logImportWidgetDiag(const QString &message)
{
    RuntimeDiagnostics::logDiag("[D3D11ImportWidget]", message);
}
}

D3D11ImportWidget::D3D11ImportWidget(QWidget *parent)
    : QOpenGLWidget(parent)
    , m_displayHost(this)
    , m_presenter(std::make_unique<QtAngleDisplayPresenter>())
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    connect(this, &QOpenGLWidget::frameSwapped, this, &D3D11ImportWidget::notifyDisplayReadyForWorker, Qt::QueuedConnection);
}

D3D11ImportWidget::~D3D11ImportWidget()
{
    m_shuttingDown = true;
    disconnect(this, &QOpenGLWidget::frameSwapped, this, &D3D11ImportWidget::notifyDisplayReadyForWorker);
    logImportWidgetDiag(QStringLiteral("Destructor begin"));
    if (context()) {
        makeCurrent();
        m_presenter->shutdown();
        doneCurrent();
    }
    logImportWidgetDiag(QStringLiteral("Destructor end"));
}

void D3D11ImportWidget::setSlotPool(const std::shared_ptr<ISharedFrameSlotPool> &slotPool)
{
    m_slotPool = slotPool;
    if (auto *presenter = dynamic_cast<QtAngleDisplayPresenter *>(m_presenter.get())) {
        presenter->setSlotPool(slotPool);
    }
}

QSize D3D11ImportWidget::outputPixelSize() const
{
    return m_displayHost.outputPixelSize();
}

void D3D11ImportWidget::initializeGL()
{
    QOpenGLFunctions *gl = context() ? context()->functions() : nullptr;
    if (gl == nullptr) {
        logImportWidgetMessage(QStringLiteral("D3D11 import widget initialization failed: Qt did not provide QOpenGLFunctions for the current context."));
        return;
    }
    gl->glDisable(GL_DEPTH_TEST);
    gl->glClearColor(0.05f, 0.06f, 0.08f, 1.0f);

    const AngleThreadingInfo angleInfo = ensureAngleD3D11MultithreadProtection();
    logImportWidgetMessage(angleInfo.message);

    QString error;
    QString runtimeLog;
    if (!m_presenter->initialize(&m_displayHost, &error, &runtimeLog)) {
        logImportWidgetMessage(QStringLiteral("D3D11 import widget initialization failed: %1\n%2")
                                   .arg(error, runtimeLog));
        return;
    }

    if (!runtimeLog.isEmpty()) {
        logImportWidgetMessage(runtimeLog);
    }
    emit outputSizeChanged(outputPixelSize());
    emit glInitialized();
    m_workerReadyPending = true;
}

void D3D11ImportWidget::resizeGL(int, int)
{
    m_presenter->onOutputSizeChanged(outputPixelSize());
    emit outputSizeChanged(outputPixelSize());
}

void D3D11ImportWidget::paintGL()
{
    QString error;
    bool releasedSlotForWorker = false;
    if (!m_presenter->paint(&error, &releasedSlotForWorker) && !error.isEmpty()) {
        logImportWidgetMessage(error);
    }
    if (releasedSlotForWorker) {
        m_workerReadyPending = true;
        emit slotAvailableForWorker();
    }
}

void D3D11ImportWidget::onFrameReady(int slotIndex, quint64 generation, QSize size, quint64 frameIndex)
{
    if (m_shuttingDown) {
        return;
    }

    PublishedFrame frame;
    frame.slotIndex = slotIndex;
    frame.generation = generation;
    frame.size = size;
    frame.frameIndex = frameIndex;
    m_presenter->consume(frame);
    m_displayHost.requestUpdate();
}

void D3D11ImportWidget::notifyDisplayReadyForWorker()
{
    if (m_shuttingDown || !m_workerReadyPending) {
        return;
    }

    m_workerReadyPending = false;
    emit displayReadyForWorker();
}
