#include "qopenglwidget_frame_view.h"

#include "angle_threading.h"
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
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    connect(this, &QOpenGLWidget::frameSwapped, this, &QOpenGLWidgetFrameView::notifyDisplayReadyForWorker, Qt::QueuedConnection);
}

QOpenGLWidgetFrameView::~QOpenGLWidgetFrameView()
{
    m_shuttingDown = true;
    disconnect(this, &QOpenGLWidget::frameSwapped, this, &QOpenGLWidgetFrameView::notifyDisplayReadyForWorker);
    logImportWidgetDiag(QStringLiteral("Destructor begin"));
    if (context() && m_presenter) {
        makeCurrent();
        m_presenter->shutdown();
        doneCurrent();
    }
    logImportWidgetDiag(QStringLiteral("Destructor end"));
}

void QOpenGLWidgetFrameView::setFrameReader(const std::shared_ptr<IFrameReader> &frameReader)
{
    m_frameReader = frameReader;
}

void QOpenGLWidgetFrameView::setPresenter(std::unique_ptr<IFramePresenter> presenter)
{
    m_presenter = std::move(presenter);
}

QSize QOpenGLWidgetFrameView::outputPixelSize() const
{
    const qreal dpr = devicePixelRatioF();
    return QSize(qMax(1, qRound(width() * dpr)),
                 qMax(1, qRound(height() * dpr)));
}

QSize QOpenGLWidgetFrameView::targetSize() const
{
    return outputPixelSize();
}

void QOpenGLWidgetFrameView::requestPresent()
{
    update();
}

QOpenGLContext *QOpenGLWidgetFrameView::glContext() const
{
    return context();
}

QOpenGLFunctions *QOpenGLWidgetFrameView::glFunctions() const
{
    QOpenGLContext *gl = context();
    return gl ? gl->functions() : nullptr;
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

    if (!m_frameReader) {
        logImportWidgetMessage(QStringLiteral("Frame view initialization failed: frame reader is not configured."));
        return;
    }
    if (!m_presenter) {
        logImportWidgetMessage(QStringLiteral("Frame view initialization failed: presenter is not configured."));
        return;
    }

    QString error;
    if (!m_presenter->initialize(*this, *m_frameReader, &error)) {
        logImportWidgetMessage(QStringLiteral("Frame view initialization failed: %1").arg(error));
        return;
    }

    const QString runtimeLog = m_presenter->diagnosticText();
    if (!runtimeLog.isEmpty()) {
        logImportWidgetMessage(runtimeLog);
    }

    emit outputSizeChanged(outputPixelSize());
    emit glInitialized();
    m_workerReadyPending = true;
}

void QOpenGLWidgetFrameView::resizeGL(int, int)
{
    emit outputSizeChanged(outputPixelSize());
}

void QOpenGLWidgetFrameView::paintGL()
{
    FramePresentationFeedback feedback;
    QString error;
    if (!m_presenter->present(&feedback, &error) && !error.isEmpty()) {
        logImportWidgetMessage(error);
    }
    if (feedback.releasedPublicationCapacity) {
        emit publicationCapacityAvailable();
    }
}

void QOpenGLWidgetFrameView::onFrameReady(const FrameTicket &ticket)
{
    if (m_shuttingDown) {
        return;
    }
    if (!m_presenter) {
        return;
    }

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
