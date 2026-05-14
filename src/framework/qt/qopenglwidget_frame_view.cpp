#include "qopenglwidget_frame_view.h"

#include "angle_threading.h"
#include "framework/qt/qt_angle_display_presenter.h"
#include "runtime_diagnostics.h"

namespace
{
void logImportWidgetMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[QOpenGLWidgetFrameView]", message);
}

void logImportWidgetDiag(const QString &message)
{
    RuntimeDiagnostics::logDiag("[QOpenGLWidgetFrameView]", message);
}
}

QOpenGLWidgetFrameView::QOpenGLWidgetFrameView(QWidget *parent)
    : QOpenGLWidget(parent)
    , m_displayHost(this)
    , m_presenter(std::make_unique<QtAngleDisplayPresenter>())
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    connect(this, &QOpenGLWidget::frameSwapped, this, &QOpenGLWidgetFrameView::notifyDisplayReadyForWorker, Qt::QueuedConnection);
}

QOpenGLWidgetFrameView::~QOpenGLWidgetFrameView()
{
    m_shuttingDown = true;
    disconnect(this, &QOpenGLWidget::frameSwapped, this, &QOpenGLWidgetFrameView::notifyDisplayReadyForWorker);
    logImportWidgetDiag(QStringLiteral("Destructor begin"));
    if (context()) {
        makeCurrent();
        m_presenter->shutdown();
        doneCurrent();
    }
    logImportWidgetDiag(QStringLiteral("Destructor end"));
}

void QOpenGLWidgetFrameView::setSlotPool(const std::shared_ptr<ISharedFrameSlotPool> &slotPool)
{
    m_slotPool = slotPool;
    if (auto *presenter = dynamic_cast<QtAngleDisplayPresenter *>(m_presenter.get())) {
        presenter->setSlotPool(slotPool);
    }
}

QSize QOpenGLWidgetFrameView::outputPixelSize() const
{
    return m_displayHost.outputPixelSize();
}

void QOpenGLWidgetFrameView::initializeGL()
{
    QOpenGLFunctions *gl = context() ? context()->functions() : nullptr;
    if (gl == nullptr) {
        logImportWidgetMessage(QStringLiteral("Frame view initialization failed: Qt did not provide QOpenGLFunctions for the current context."));
        return;
    }
    gl->glDisable(GL_DEPTH_TEST);
    gl->glClearColor(0.05f, 0.06f, 0.08f, 1.0f);

    const AngleThreadingInfo angleInfo = ensureAngleD3D11MultithreadProtection();
    logImportWidgetMessage(angleInfo.message);

    QString error;
    if (!m_presenter->initialize(m_displayHost, &error)) {
        logImportWidgetMessage(QStringLiteral("Frame view initialization failed: %1").arg(error));
        return;
    }

    if (auto *presenter = dynamic_cast<QtAngleDisplayPresenter *>(m_presenter.get())) {
        const QString runtimeLog = presenter->lastRuntimeLog();
        if (!runtimeLog.isEmpty()) {
            logImportWidgetMessage(runtimeLog);
        }
    }

    emit outputSizeChanged(outputPixelSize());
    emit glInitialized();
    m_workerReadyPending = true;
}

void QOpenGLWidgetFrameView::resizeGL(int, int)
{
    if (auto *presenter = dynamic_cast<QtAngleDisplayPresenter *>(m_presenter.get())) {
        presenter->onOutputSizeChanged(outputPixelSize());
    }
    emit outputSizeChanged(outputPixelSize());
}

void QOpenGLWidgetFrameView::paintGL()
{
    PresentationFeedback feedback;
    QString error;
    if (!m_presenter->present(&feedback, &error) && !error.isEmpty()) {
        logImportWidgetMessage(error);
    }
    if (feedback.releasedPublicationCapacity) {
        emit publicationCapacityAvailable();
    }
}

void QOpenGLWidgetFrameView::onFrameReady(int slotIndex, quint64 generation, QSize size, quint64 frameIndex)
{
    if (m_shuttingDown) {
        return;
    }

    PublicationTicket ticket;
    ticket.transportMetadata.insert(QStringLiteral("slotIndex"), slotIndex);
    ticket.transportMetadata.insert(QStringLiteral("generation"), qulonglong(generation));
    ticket.transportMetadata.insert(QStringLiteral("frameIndex"), qulonglong(frameIndex));
    ticket.transportMetadata.insert(QStringLiteral("size"), size);
    ticket.artifact.logicalSize = size;

    QString error;
    if (!m_presenter->enqueue(ticket, &error) && !error.isEmpty()) {
        logImportWidgetMessage(error);
        return;
    }
}

void QOpenGLWidgetFrameView::notifyDisplayReadyForWorker()
{
    if (m_shuttingDown || !m_workerReadyPending) {
        return;
    }

    m_workerReadyPending = false;
    emit displayReadyForWorker();
}
