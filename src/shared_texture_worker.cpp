#include "shared_texture_worker.h"

#include "angle_threading.h"
#include "gles_thread_guard.h"
#include "image_processing_pipeline.h"
#include "shared_gl_context_handle.h"
#include "shared_texture_frame_pool.h"

#include <QDebug>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QThread>
#include <QTimer>

namespace
{
constexpr int kMakeCurrentRetryCount = 5;
constexpr int kMakeCurrentRetryDelayMs = 20;

QString formatToString(const QSurfaceFormat &format)
{
    QString text;
    QDebug debug(&text);
    debug.nospace() << format;
    return text;
}

QString threadIdToString(Qt::HANDLE threadId)
{
    return QStringLiteral("0x%1")
        .arg(quintptr(threadId), QT_POINTER_SIZE * 2, 16, QLatin1Char('0'));
}
}

SharedTextureWorker::SharedTextureWorker(QObject *parent)
    : QObject(parent)
    , m_pipeline(std::make_unique<ImageProcessingPipeline>())
{
}

SharedTextureWorker::~SharedTextureWorker()
{
    shutdown();
}

bool SharedTextureWorker::initialize(SharedGlContextHandle *handle,
                                     SharedTextureFramePool *framePool,
                                     QSize outputSize)
{
    if (m_initialized || handle == nullptr || framePool == nullptr || framePool->slotCount() <= 0) {
        delete handle;
        return false;
    }

    m_handle = handle;
    m_framePool = framePool;
    m_sharedTextures = QVector<GLuint>(m_framePool->slotCount(), 0U);
    m_allocatedSizes = QVector<QSize>(m_framePool->slotCount());

    {
        QMutexLocker locker(&m_stateMutex);
        m_outputSize = outputSize.expandedTo(QSize(1, 1));
    }

    qInfo() << "Worker context created. shareGroup=" << m_handle->context()->shareGroup()
            << "isOpenGLES=" << m_handle->context()->isOpenGLES()
            << "format=" << m_handle->context()->format();

    QString error;
    if (!makeWorkerContextCurrent("initialization", &error)) {
        emit initializationFailed(error);
        delete m_handle;
        m_handle = nullptr;
        m_framePool = nullptr;
        return false;
    }

    const AngleThreadingInfo angleInfo = ensureAngleD3D11MultithreadProtection();
    emit statusMessage(angleInfo.message);

    if (!m_pipeline->initialize(m_handle->context(), &error)) {
        m_handle->doneCurrent();
        sharedGlesMutex().unlock();
        emit initializationFailed(error);
        delete m_handle;
        m_handle = nullptr;
        m_framePool = nullptr;
        return false;
    }

    QOpenGLFunctions *gl = m_handle->context()->functions();
    if (!m_sharedTextures.isEmpty()) {
        gl->glGenTextures(m_sharedTextures.size(), m_sharedTextures.data());
        if (m_sharedTextures[0] == 0U) {
            m_handle->doneCurrent();
            sharedGlesMutex().unlock();
            emit initializationFailed(QStringLiteral("Failed to create shared textures for worker slots."));
            delete m_handle;
            m_handle = nullptr;
            m_framePool = nullptr;
            return false;
        }

        for (int i = 0; i < m_sharedTextures.size(); ++i) {
            m_framePool->registerTexture(i, m_sharedTextures[i]);
        }
    }

    m_framePool->reset();

    m_handle->doneCurrent();
    sharedGlesMutex().unlock();

    m_initialized = true;
    return true;
}

void SharedTextureWorker::setOutputSize(QSize size)
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_outputSize = size.expandedTo(QSize(1, 1));
    }
    scheduleRender(0);
}

void SharedTextureWorker::loadImageDirectory(const QString &directoryPath)
{
    if (!m_initialized || m_handle == nullptr || m_pipeline == nullptr) {
        emit imageDirectoryLoadFinished(
            false,
            QStringLiteral("Worker OpenGL context is not initialized."),
            -1,
            0,
            {});
        return;
    }

    QString error;
    if (!makeWorkerContextCurrent("loadImageDirectory", &error)) {
        emit imageDirectoryLoadFinished(false, error, -1, 0, {});
        return;
    }

    const bool loaded = m_pipeline->loadImageDirectory(m_handle->context(), directoryPath, &error);
    if (loaded && m_handle->context()->functions() != nullptr) {
        m_handle->context()->functions()->glFinish();
    }

    m_handle->doneCurrent();
    sharedGlesMutex().unlock();

    if (!loaded) {
        emit imageDirectoryLoadFinished(false, error, -1, 0, {});
        return;
    }

    emit imageDirectoryLoadFinished(
        true,
        {},
        m_pipeline->currentIndex(),
        m_pipeline->imageCount(),
        m_pipeline->currentDisplayName());
    emitImageSelection();
    scheduleRender(0);
}

void SharedTextureWorker::selectNextImage()
{
    if (!m_initialized || m_pipeline == nullptr || !m_pipeline->selectNextImage()) {
        return;
    }

    emitImageSelection();
    scheduleRender(0);
}

void SharedTextureWorker::selectPreviousImage()
{
    if (!m_initialized || m_pipeline == nullptr || !m_pipeline->selectPreviousImage()) {
        return;
    }

    emitImageSelection();
    scheduleRender(0);
}

void SharedTextureWorker::setEffectParameters(const ImageEffectParameters &parameters)
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_effectParameters = parameters;
    }
    scheduleRender(0);
}

void SharedTextureWorker::requestRender()
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_renderScheduled = false;
    }

    if (!m_initialized || m_handle == nullptr || m_pipeline == nullptr || !m_pipeline->hasImage()) {
        return;
    }

    const QSize size = currentOutputSize();
    int renderSlot = -1;
    if (m_framePool == nullptr || !m_framePool->tryAcquireRenderSlot(&renderSlot)) {
        scheduleRender(4);
        return;
    }

    QString error;
    if (!makeWorkerContextCurrent("processing", &error)) {
        m_framePool->abandonRenderSlot(renderSlot);
        emit initializationFailed(error);
        return;
    }

    if (!ensureSharedTextureForSlot(renderSlot, size, &error)) {
        m_handle->doneCurrent();
        sharedGlesMutex().unlock();
        m_framePool->abandonRenderSlot(renderSlot);
        emit initializationFailed(error);
        return;
    }

    const ImageEffectParameters parameters = [this]() {
        QMutexLocker locker(&m_stateMutex);
        return m_effectParameters;
    }();

    const GLuint textureId = m_framePool->textureIdForSlot(renderSlot);
    const bool rendered = m_pipeline->renderToTexture(
        m_handle->context(),
        textureId,
        size,
        parameters,
        &error);
    if (!rendered) {
        m_handle->doneCurrent();
        sharedGlesMutex().unlock();
        m_framePool->abandonRenderSlot(renderSlot);
        emit initializationFailed(error.isEmpty()
                                      ? QStringLiteral("Image processing pipeline failed to render a frame.")
                                      : error);
        return;
    }

    if (m_handle->context()->functions() != nullptr) {
        m_handle->context()->functions()->glFinish();
    }

    m_handle->doneCurrent();
    sharedGlesMutex().unlock();

    SharedTextureFrame frame;
    if (m_framePool->submitRenderedFrame(renderSlot, size, m_frameIndex, &frame)) {
        emit textureReady(frame.slotIndex, frame.textureId, frame.size, frame.frameIndex);
        ++m_frameIndex;
        return;
    }

    m_framePool->abandonRenderSlot(renderSlot);
    scheduleRender(4);
}

void SharedTextureWorker::shutdown()
{
    if (m_handle != nullptr && m_pipeline != nullptr && makeWorkerContextCurrent("shutdown", nullptr)) {
        if (!m_sharedTextures.isEmpty()) {
            m_handle->context()->functions()->glDeleteTextures(m_sharedTextures.size(), m_sharedTextures.data());
        }
        m_pipeline->release(m_handle->context());
        m_handle->doneCurrent();
        sharedGlesMutex().unlock();
    }

    delete m_handle;
    m_handle = nullptr;
    m_framePool = nullptr;
    m_sharedTextures.clear();
    m_allocatedSizes.clear();
    m_initialized = false;
    m_renderScheduled = false;
    m_frameIndex = 0;
}

QSize SharedTextureWorker::currentOutputSize() const
{
    QMutexLocker locker(&m_stateMutex);
    return m_outputSize.expandedTo(QSize(1, 1));
}

bool SharedTextureWorker::makeWorkerContextCurrent(const char *phase, QString *error)
{
    if (m_handle == nullptr || m_handle->context() == nullptr || m_handle->surface() == nullptr) {
        if (error) {
            *error = QStringLiteral("Worker context handle is incomplete.");
        }
        return false;
    }

    for (int attempt = 1; attempt <= kMakeCurrentRetryCount; ++attempt) {
        sharedGlesMutex().lock();
        if (m_handle->makeCurrent()) {
            return true;
        }
        sharedGlesMutex().unlock();

        qWarning().noquote()
            << QStringLiteral("Worker makeCurrent failed during %1 (attempt %2/%3). %4")
                   .arg(QString::fromLatin1(phase))
                   .arg(attempt)
                   .arg(kMakeCurrentRetryCount)
                   .arg(describeContextState());

        if (attempt < kMakeCurrentRetryCount) {
            QThread::msleep(kMakeCurrentRetryDelayMs);
        }
    }

    if (error) {
        *error = QStringLiteral("Failed to make worker context current during %1. %2")
                     .arg(QString::fromLatin1(phase), describeContextState());
    }
    return false;
}

bool SharedTextureWorker::ensureSharedTextureForSlot(int slotIndex, const QSize &size, QString *error)
{
    if (m_handle == nullptr || m_handle->context() == nullptr || m_framePool == nullptr) {
        if (error) {
            *error = QStringLiteral("Worker shared texture prerequisites are incomplete.");
        }
        return false;
    }

    if (slotIndex < 0 || slotIndex >= m_sharedTextures.size()) {
        if (error) {
            *error = QStringLiteral("Shared texture slot index is out of range.");
        }
        return false;
    }

    QOpenGLFunctions *gl = m_handle->context()->functions();
    if (m_sharedTextures[slotIndex] == 0U) {
        gl->glGenTextures(1, &m_sharedTextures[slotIndex]);
        if (m_sharedTextures[slotIndex] == 0U) {
            if (error) {
                *error = QStringLiteral("Failed to allocate a shared texture for the worker slot.");
            }
            return false;
        }
        m_framePool->registerTexture(slotIndex, m_sharedTextures[slotIndex]);
    }

    if (m_allocatedSizes[slotIndex] == size) {
        return true;
    }

    gl->glBindTexture(GL_TEXTURE_2D, m_sharedTextures[slotIndex]);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA,
                     size.width(),
                     size.height(),
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     nullptr);
    gl->glBindTexture(GL_TEXTURE_2D, 0);

    const GLenum glError = gl->glGetError();
    if (glError != GL_NO_ERROR) {
        if (error) {
            *error = QStringLiteral("Shared texture allocation failed with GL error 0x%1")
                         .arg(static_cast<unsigned int>(glError), 0, 16);
        }
        return false;
    }

    m_allocatedSizes[slotIndex] = size;
    return true;
}

QString SharedTextureWorker::describeContextState() const
{
    return QStringLiteral("workerThread=%1 contextValid=%2 contextFormat=%3")
        .arg(threadIdToString(QThread::currentThreadId()),
             m_handle != nullptr && m_handle->context() != nullptr && m_handle->context()->isValid()
                 ? QStringLiteral("true")
                 : QStringLiteral("false"),
             m_handle != nullptr && m_handle->context() != nullptr
                 ? formatToString(m_handle->context()->format())
                 : QStringLiteral("<null>"));
}

void SharedTextureWorker::scheduleRender(int delayMs)
{
    QMutexLocker locker(&m_stateMutex);
    if (m_renderScheduled) {
        return;
    }

    m_renderScheduled = true;
    QTimer::singleShot(qMax(0, delayMs), this, &SharedTextureWorker::requestRender);
}

void SharedTextureWorker::emitImageSelection()
{
    if (m_pipeline == nullptr || !m_pipeline->hasImage()) {
        emit imageSelectionChanged(-1, 0, {});
        return;
    }

    emit imageSelectionChanged(
        m_pipeline->currentIndex(),
        m_pipeline->imageCount(),
        m_pipeline->currentDisplayName());
}
