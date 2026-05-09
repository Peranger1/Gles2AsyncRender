#include "async_gles_widget.h"

#include "angle_threading.h"
#include "gles_thread_guard.h"
#include "shared_gl_environment.h"
#include "shared_texture_frame_pool.h"

#include <QDebug>
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
    , m_framePool(std::make_shared<SharedTextureFramePool>(3))
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

    connect(this, &QOpenGLWidget::aboutToCompose, this, &AsyncGlesWidget::lockForComposition, Qt::DirectConnection);
    connect(this, &QOpenGLWidget::aboutToCompose, this, &AsyncGlesWidget::onAboutToCompose, Qt::DirectConnection);
    connect(this, &QOpenGLWidget::frameSwapped, this, &AsyncGlesWidget::unlockForComposition, Qt::DirectConnection);
    connect(this, &QOpenGLWidget::frameSwapped, this, &AsyncGlesWidget::notifyDisplayReadyForWorker, Qt::QueuedConnection);
    connect(this, &QOpenGLWidget::frameSwapped, this, &AsyncGlesWidget::onFrameSwapped, Qt::DirectConnection);
}

AsyncGlesWidget::~AsyncGlesWidget()
{
    unlockForComposition();

    delete m_pendingFrame;
    m_pendingFrame = nullptr;

    if (context()) {
        ScopedGlesLock lock;
        makeCurrent();
        if (m_framePool) {
            m_framePool->releaseAllFences(context());
        }
        m_program.removeAllShaders();
        doneCurrent();
    }
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

void AsyncGlesWidget::setSharedGlEnvironment(SharedGlEnvironment *environment)
{
    m_sharedGlEnvironment = environment;
}

QSize AsyncGlesWidget::outputPixelSize() const
{
    const qreal dpr = devicePixelRatioF();
    return QSize(qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
}

SharedTextureFramePool *AsyncGlesWidget::framePool() const
{
    return m_framePool.get();
}

void AsyncGlesWidget::initializeGL()
{
    ScopedGlesLock lock;
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

    if (m_sharedGlEnvironment != nullptr) {
        m_sharedGlEnvironment->initializeFromDisplay(context(), format());
    }

    m_acceptFrames = true;
    m_workerReadyPending = true;
    if (m_framePool) {
        m_framePool->reset();
    }
    emit outputSizeChanged(outputPixelSize());
    emit glInitialized();
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
    ScopedGlesLock lock;
    emit outputSizeChanged(outputPixelSize());
}

void AsyncGlesWidget::paintGL()
{
    ScopedGlesLock lock;

    if (m_pendingFrame && m_framePool) {
        int retiredSlot = -1;
        SharedTextureFrame promotedFrame;
        QString error;
        if (m_framePool->consumePendingFrame(
                m_pendingFrame->slotIndex,
                context(),
                &retiredSlot,
                &promotedFrame,
                &error)) {
            m_frontSlot = promotedFrame.slotIndex;
            m_displayTexture = promotedFrame.textureId;
            m_displayTextureSize = promotedFrame.size;
            m_displayFrame = promotedFrame.frameIndex;
            m_retiringSlot = retiredSlot;
            delete m_pendingFrame;
            m_pendingFrame = nullptr;
        } else if (!error.isEmpty()) {
            qWarning() << "Failed to consume pending frame:" << error;
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

void AsyncGlesWidget::notifyDisplayReadyForWorker()
{
    if (!m_workerReadyPending) {
        return;
    }

    m_workerReadyPending = false;
    emit displayReadyForWorker();
}

void AsyncGlesWidget::onAboutToCompose()
{
    if (m_framePool) {
        m_framePool->markFrontSlotComposing(true);
    }
}

void AsyncGlesWidget::onFrameSwapped()
{
    if (m_framePool) {
        m_framePool->markFrontSlotComposing(false);
    }

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
