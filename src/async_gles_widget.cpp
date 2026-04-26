#include "async_gles_widget.h"

#include "angle_threading.h"
#include "gles_thread_guard.h"
#include "shared_texture_frame_pool.h"
#include "shared_texture_worker.h"

#include <QDebug>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPaintEvent>
#include <QResizeEvent>

namespace
{
constexpr GLfloat kVertices[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
    -1.0f,  1.0f,
     1.0f,  1.0f
};

constexpr GLfloat kTexCoords[] = {
    0.0f, 1.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 0.0f
};
} // namespace

AsyncGlesWidget::AsyncGlesWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

    connect(this, &QOpenGLWidget::aboutToCompose, this, &AsyncGlesWidget::lockForComposition, Qt::DirectConnection);
    connect(this, &QOpenGLWidget::frameSwapped, this, &AsyncGlesWidget::unlockForComposition, Qt::DirectConnection);
    connect(this, &QOpenGLWidget::aboutToResize, this, &AsyncGlesWidget::lockForComposition, Qt::DirectConnection);
    connect(this, &QOpenGLWidget::resized, this, &AsyncGlesWidget::unlockForComposition, Qt::DirectConnection);
    connect(this, &QOpenGLWidget::frameSwapped, this, &AsyncGlesWidget::startWorkerIfNeeded, Qt::QueuedConnection);
    connect(this, &QOpenGLWidget::frameSwapped, this, &AsyncGlesWidget::onFrameSwapped, Qt::DirectConnection);
}

AsyncGlesWidget::~AsyncGlesWidget()
{
    stopWorker();
    unlockForComposition();

    if (context()) {
        ScopedGlesLock lock;
        makeCurrent();
        m_program.removeAllShaders();
        doneCurrent();
    }
}

QSize AsyncGlesWidget::outputPixelSize() const
{
    const qreal dpr = devicePixelRatioF();
    return QSize(qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
}

bool AsyncGlesWidget::createProgram()
{
    static const char *vertexShader = R"(
        attribute highp vec2 aPosition;
        attribute mediump vec2 aTexCoord;
        varying mediump vec2 vTexCoord;

        void main()
        {
            vTexCoord = aTexCoord;
            gl_Position = vec4(aPosition, 0.0, 1.0);
        }
    )";

    static const char *fragmentShader = R"(
        varying mediump vec2 vTexCoord;
        uniform sampler2D uTexture;

        void main()
        {
            gl_FragColor = texture2D(uTexture, vTexCoord);
        }
    )";

    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)) {
        qWarning() << "Vertex shader compile failed:" << m_program.log();
        return false;
    }

    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)) {
        qWarning() << "Fragment shader compile failed:" << m_program.log();
        return false;
    }

    if (!m_program.link()) {
        qWarning() << "Shader link failed:" << m_program.log();
        return false;
    }

    m_positionLocation = m_program.attributeLocation("aPosition");
    m_texCoordLocation = m_program.attributeLocation("aTexCoord");
    m_samplerLocation = m_program.uniformLocation("uTexture");
    return m_positionLocation >= 0 && m_texCoordLocation >= 0 && m_samplerLocation >= 0;
}

void AsyncGlesWidget::initializeGL()
{
    ScopedGlesLock lock;

    stopWorker();

    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.08f, 0.09f, 0.11f, 1.0f);

    qInfo() << "Widget context created. shareGroup=" << context()->shareGroup()
            << "isOpenGLES=" << context()->isOpenGLES()
            << "format=" << context()->format();

    const AngleThreadingInfo angleInfo = ensureAngleD3D11MultithreadProtection();
    qInfo().noquote() << angleInfo.message;

    if (!createProgram()) {
        return;
    }

    m_surface = std::make_unique<QOffscreenSurface>();
    m_surface->setScreen(context()->screen());
    m_surface->setFormat(context()->format());
    m_surface->create();
    if (!m_surface->isValid()) {
        qWarning() << "Failed to create offscreen surface for worker.";
        return;
    }

    m_framePool = std::make_shared<SharedTextureFramePool>(3);
    m_worker = std::make_unique<SharedTextureWorker>(context(), m_surface.get(), m_framePool, context()->format(), this);
    connect(m_worker.get(), &SharedTextureWorker::textureReady, this, &AsyncGlesWidget::onTextureReady);
    connect(m_worker.get(), &SharedTextureWorker::initializationFailed, this, &AsyncGlesWidget::onWorkerError);
    connect(m_worker.get(), &SharedTextureWorker::statusMessage, this, &AsyncGlesWidget::onWorkerStatus);

    m_acceptFrames = true;
    m_workerStartPending = true;
    m_worker->setOutputSize(outputPixelSize());
}

void AsyncGlesWidget::paintEvent(QPaintEvent *event)
{
    ScopedGlesLock lock;
    QOpenGLWidget::paintEvent(event);
}

void AsyncGlesWidget::resizeEvent(QResizeEvent *event)
{
    ScopedGlesLock lock;
    QOpenGLWidget::resizeEvent(event);
}

void AsyncGlesWidget::resizeGL(int, int)
{
    if (m_worker) {
        m_worker->setOutputSize(outputPixelSize());
    }
}

void AsyncGlesWidget::paintGL()
{
    ScopedGlesLock lock;

    if (m_pendingFrame && m_retiringSlot == -1 && m_framePool) {
        int retiredSlot = -1;
        if (m_framePool->promotePendingFrame(m_pendingFrame->slotIndex, &retiredSlot)) {
            m_frontSlot = m_pendingFrame->slotIndex;
            m_displayTexture = m_pendingFrame->textureId;
            m_displayTextureSize = m_pendingFrame->size;
            m_displayFrame = m_pendingFrame->frameIndex;
            m_retiringSlot = retiredSlot;
            delete m_pendingFrame;
            m_pendingFrame = nullptr;
        }
    }

    glViewport(0, 0, outputPixelSize().width(), outputPixelSize().height());
    glClear(GL_COLOR_BUFFER_BIT);

    if (m_displayTexture == 0U || !m_program.isLinked()) {
        return;
    }

    m_program.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_displayTexture);
    glUniform1i(m_samplerLocation, 0);

    glVertexAttribPointer(m_positionLocation, 2, GL_FLOAT, GL_FALSE, 0, kVertices);
    glEnableVertexAttribArray(m_positionLocation);
    glVertexAttribPointer(m_texCoordLocation, 2, GL_FLOAT, GL_FALSE, 0, kTexCoords);
    glEnableVertexAttribArray(m_texCoordLocation);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(m_positionLocation);
    glDisableVertexAttribArray(m_texCoordLocation);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_program.release();
}

void AsyncGlesWidget::onTextureReady(int slotIndex, quint32 textureId, QSize size, quint64 frameIndex)
{
    if (!m_acceptFrames) {
        return;
    }

    delete m_pendingFrame;
    m_pendingFrame = new SharedTextureFrame{slotIndex, textureId, size, frameIndex};
    update();
}

void AsyncGlesWidget::onWorkerError(const QString &reason)
{
    qWarning() << "Shared texture worker error:" << reason;
}

void AsyncGlesWidget::onWorkerStatus(const QString &message)
{
    qInfo().noquote() << message;
}

void AsyncGlesWidget::startWorkerIfNeeded()
{
    if (!m_workerStartPending || !m_worker || m_worker->isRunning()) {
        return;
    }

    m_worker->setOutputSize(outputPixelSize());
    m_workerStartPending = false;
    m_worker->start();
}

void AsyncGlesWidget::onFrameSwapped()
{
    if (m_retiringSlot != -1 && m_framePool) {
        m_framePool->releaseRetiredSlot(m_retiringSlot);
        m_retiringSlot = -1;
    }

    if (m_pendingFrame) {
        update();
    }
}

void AsyncGlesWidget::lockForComposition()
{
    if (m_compositionLocked) {
        return;
    }

    sharedGlesMutex().lock();
    m_compositionLocked = true;
}

void AsyncGlesWidget::unlockForComposition()
{
    if (!m_compositionLocked) {
        return;
    }

    m_compositionLocked = false;
    sharedGlesMutex().unlock();
}

void AsyncGlesWidget::stopWorker()
{
    m_acceptFrames = false;
    m_workerStartPending = false;

    delete m_pendingFrame;
    m_pendingFrame = nullptr;

    if (m_worker) {
        disconnect(m_worker.get(), nullptr, this, nullptr);
        m_worker->stop();
        m_worker.reset();
    }
    m_framePool.reset();

    m_displayTexture = 0U;
    m_displayTextureSize = QSize();
    m_displayFrame = 0U;
    m_frontSlot = -1;
    m_retiringSlot = -1;
}
