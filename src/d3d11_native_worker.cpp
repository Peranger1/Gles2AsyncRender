#include "d3d11_native_worker.h"

#include "angle_shared_texture_publish_bridge.h"
#include "d3d11_native_slot_pool.h"
#include "photo_editor_gles2_simulator.h"
#include "photo_editor_library_host.h"
#include "photo_editor_session.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <QDebug>

namespace
{
void logWorkerMessage(const QString &message)
{
    if (!message.isEmpty()) {
        qInfo().noquote() << "[D3D11NativeWorker]" << message;
    }
}

QString formatParameters(const ImageEffectParameters &parameters)
{
    return QStringLiteral("brightness=%1 contrast=%2 zoom=%3 pan=(%4,%5) rotation=%6 flipH=%7 flipV=%8 heavyPass=%9")
        .arg(QString::number(parameters.brightness, 'f', 3))
        .arg(QString::number(parameters.contrast, 'f', 3))
        .arg(QString::number(parameters.zoom, 'f', 3))
        .arg(QString::number(parameters.panX, 'f', 3))
        .arg(QString::number(parameters.panY, 'f', 3))
        .arg(QString::number(parameters.rotationDegrees, 'f', 3))
        .arg(parameters.flipHorizontal)
        .arg(parameters.flipVertical)
        .arg(parameters.heavyGpuPassCount);
}

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

void processProgressThunk(int progress, bool isEnd, void *userData)
{
    D3D11NativeWorker *worker = static_cast<D3D11NativeWorker *>(userData);
    if (worker == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(
        worker,
        "onProcessProgressEvent",
        Qt::QueuedConnection,
        Q_ARG(int, progress),
        Q_ARG(bool, isEnd));
}
} // namespace

struct D3D11NativeWorker::Impl final
{
    QSurfaceFormat glFormat;
    std::unique_ptr<QOffscreenSurface> librarySurface;
    std::unique_ptr<QOpenGLContext> libraryContext;
    PhotoEditorLibraryHost libraryHost;
    AngleSharedTexturePublishBridge publishBridge;
    PhotoEditorSession session;

    QStringList imagePaths;
    int currentImageIndex = -1;
    QImage currentImage;

    bool publishActive = false;
    bool shutdownRequested = false;

    QStringList collectImages(const QString &directoryPath) const
    {
        QDir directory(directoryPath);
        const QStringList filters = {
            QStringLiteral("*.png"),
            QStringLiteral("*.jpg"),
            QStringLiteral("*.jpeg"),
            QStringLiteral("*.bmp"),
            QStringLiteral("*.webp")
        };

        QStringList result;
        const QFileInfoList entries = directory.entryInfoList(
            filters,
            QDir::Files | QDir::Readable | QDir::NoSymLinks,
            QDir::Name);
        for (const QFileInfo &entry : entries) {
            result.push_back(entry.absoluteFilePath());
        }
        return result;
    }

    QString currentDisplayName() const
    {
        if (currentImageIndex < 0 || currentImageIndex >= imagePaths.size()) {
            return {};
        }
        return QFileInfo(imagePaths[currentImageIndex]).fileName();
    }

    bool hasImage() const
    {
        return !currentImage.isNull();
    }

    bool initialize(D3D11NativeSlotPool *slotPool, const QSize &outputSize, QString *error)
    {
        Q_UNUSED(outputSize);
        glFormat = QSurfaceFormat::defaultFormat();

        librarySurface = std::make_unique<QOffscreenSurface>();
        librarySurface->setFormat(glFormat);
        librarySurface->create();
        if (!librarySurface->isValid()) {
            if (error) {
                *error = QStringLiteral("The algorithm library offscreen surface is invalid.");
            }
            return false;
        }

        libraryContext = std::make_unique<QOpenGLContext>();
        libraryContext->setFormat(glFormat);
        if (!libraryContext->create()) {
            if (error) {
                *error = QStringLiteral("The algorithm library OpenGL context could not be created.");
            }
            return false;
        }

        if (!libraryContext->makeCurrent(librarySurface.get())) {
            if (error) {
                *error = QStringLiteral("The algorithm library OpenGL context could not be made current.");
            }
            return false;
        }

        const bool initialized = libraryHost.initializeOnce(libraryContext.get(), error)
            && publishBridge.initialize(libraryContext.get(), slotPool, error);
        libraryContext->doneCurrent();
        if (!initialized) {
            return false;
        }

        session.reset();
        session.latestParameters = {};
        session.processingParameters = {};
        session.hasLatestParameters = true;
        return true;
    }

    void destroySession()
    {
        if (session.handle == nullptr) {
            session.reset();
            return;
        }

        const bool hasGlTarget = libraryContext && librarySurface;
        const bool madeCurrent = hasGlTarget && libraryContext->makeCurrent(librarySurface.get());
        photo_editor_destroy(session.handle);
        if (madeCurrent) {
            libraryContext->doneCurrent();
        }
        session.reset();
    }

    bool rebuildSessionForCurrentImage(const QSize &outputSize, QString *error)
    {
        destroySession();
        if (currentImage.isNull()) {
            return true;
        }
        if (!libraryContext || !librarySurface) {
            if (error) {
                *error = QStringLiteral("The algorithm library context is not initialized.");
            }
            return false;
        }

        if (!libraryContext->makeCurrent(librarySurface.get())) {
            if (error) {
                *error = QStringLiteral("The algorithm library context could not be made current for session creation.");
            }
            return false;
        }

        session.handle = photo_editor_create(currentImage, error);
        bool ok = session.handle != nullptr;
        if (ok) {
            ok = photo_editor_set_output_size(session.handle, sanitizedSize(outputSize), error);
        }
        libraryContext->doneCurrent();
        if (!ok) {
            if (session.handle != nullptr) {
                if (libraryContext->makeCurrent(librarySurface.get())) {
                    photo_editor_destroy(session.handle);
                    libraryContext->doneCurrent();
                }
            }
            session.reset();
            return false;
        }

        session.latestParameters = {};
        session.processingParameters = {};
        session.hasLatestParameters = true;
        session.parametersDirty = false;
        session.processInFlight = false;
        session.renderReady = false;
        session.latestProgress = 0;
        return true;
    }

    bool loadImageDirectory(const QString &directoryPath, const QSize &outputSize, QString *error)
    {
        const QStringList paths = collectImages(directoryPath);
        if (paths.isEmpty()) {
            if (error) {
                *error = QStringLiteral("No supported images were found in the selected directory.");
            }
            return false;
        }

        imagePaths = paths;
        currentImageIndex = 0;
        currentImage = QImage(paths.first());
        if (currentImage.isNull()) {
            if (error) {
                *error = QStringLiteral("Failed to load the first image from the selected directory.");
            }
            return false;
        }

        return rebuildSessionForCurrentImage(outputSize, error);
    }

    bool selectRelativeImage(int delta, const QSize &outputSize, QString *error)
    {
        if (imagePaths.isEmpty()) {
            return false;
        }

        const int count = imagePaths.size();
        currentImageIndex = (currentImageIndex + delta + count) % count;
        currentImage = QImage(imagePaths[currentImageIndex]);
        if (currentImage.isNull()) {
            if (error) {
                *error = QStringLiteral("Failed to load image: %1").arg(imagePaths[currentImageIndex]);
            }
            return false;
        }

        return rebuildSessionForCurrentImage(outputSize, error);
    }

    bool updateOutputSize(const QSize &outputSize, QString *error)
    {
        if (session.handle == nullptr || !libraryContext || !librarySurface) {
            return true;
        }

        if (!libraryContext->makeCurrent(librarySurface.get())) {
            if (error) {
                *error = QStringLiteral("The algorithm library context could not be made current for resize.");
            }
            return false;
        }

        const bool ok = photo_editor_set_output_size(session.handle, sanitizedSize(outputSize), error);
        libraryContext->doneCurrent();
        return ok;
    }

    bool startProcess(const ImageEffectParameters &parameters,
                      const QSize &outputSize,
                      QObject *callbackContext,
                      void *callbackUserData,
                      QString *error)
    {
        if (session.handle == nullptr) {
            if (error) {
                *error = QStringLiteral("No active image session exists.");
            }
            return false;
        }
        if (session.processInFlight || publishActive) {
            if (error) {
                *error = QStringLiteral("The session is still busy.");
            }
            return false;
        }
        if (!libraryContext || !librarySurface) {
            if (error) {
                *error = QStringLiteral("The algorithm library context is not initialized.");
            }
            return false;
        }

        if (!libraryContext->makeCurrent(librarySurface.get())) {
            if (error) {
                *error = QStringLiteral("The algorithm library context could not be made current for process start.");
            }
            return false;
        }

        logWorkerMessage(QStringLiteral("[diag] startProcess begin ctx=%1 thread=%2 imageIndex=%3 output=%4x%5 %6")
                             .arg(reinterpret_cast<quintptr>(libraryContext.get()), 0, 16)
                             .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16)
                             .arg(currentImageIndex)
                             .arg(outputSize.width())
                             .arg(outputSize.height())
                             .arg(formatParameters(parameters)));

        bool ok = photo_editor_set_output_size(session.handle, sanitizedSize(outputSize), error);
        if (ok) {
            ok = photo_editor_set_opcode(session.handle, parameters, error);
        }
        if (ok) {
            ok = photo_editor_process(session.handle,
                                      callbackContext,
                                      &processProgressThunk,
                                      callbackUserData,
                                      error);
        }
        libraryContext->doneCurrent();

        if (ok) {
            session.processingParameters = parameters;
            session.processInFlight = true;
            session.renderReady = false;
            session.latestProgress = 0;
            logWorkerMessage(QStringLiteral("[diag] startProcess queued ctx=%1 processInFlight=%2 renderReady=%3")
                                 .arg(reinterpret_cast<quintptr>(libraryContext.get()), 0, 16)
                                 .arg(session.processInFlight)
                                 .arg(session.renderReady));
        } else {
            logWorkerMessage(QStringLiteral("[diag] startProcess failed error=%1")
                                 .arg(error ? *error : QString()));
        }
        return ok;
    }

    bool renderAndPublish(const QSize &outputSize,
                          D3D11NativeSlotPool *slotPool,
                          quint64 frameIndex,
                          D3D11NativeFrame *frame,
                          QString *error)
    {
        if (session.handle == nullptr || !session.renderReady) {
            if (error) {
                *error = QStringLiteral("No completed algorithm result is ready for publish.");
            }
            return false;
        }

        int renderSlot = -1;
        if (!slotPool->tryAcquireRenderSlot(&renderSlot)) {
            return false;
        }

        publishActive = true;

        if (!libraryContext || !librarySurface) {
            slotPool->abandonRenderSlot(renderSlot);
            publishActive = false;
            if (error) {
                *error = QStringLiteral("The algorithm library context is not initialized.");
            }
            return false;
        }

        if (!libraryContext->makeCurrent(librarySurface.get())) {
            slotPool->abandonRenderSlot(renderSlot);
            publishActive = false;
            if (error) {
                *error = QStringLiteral("The algorithm library context could not be made current for render.");
            }
            return false;
        }

        logWorkerMessage(QStringLiteral("[diag] renderAndPublish begin frame=%1 slot=%2 ctx=%3 thread=%4 renderReady=%5 latestProgress=%6 output=%7x%8")
                             .arg(frameIndex)
                             .arg(renderSlot)
                             .arg(reinterpret_cast<quintptr>(libraryContext.get()), 0, 16)
                             .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16)
                             .arg(session.renderReady)
                             .arg(session.latestProgress)
                             .arg(outputSize.width())
                             .arg(outputSize.height()));

        bool ok = photo_editor_set_output_size(session.handle, sanitizedSize(outputSize), error);
        GLuint textureId = 0;
        QSize textureSize;
        if (ok) {
            ok = photo_editor_render(session.handle, &textureId, &textureSize, error);
            logWorkerMessage(QStringLiteral("[diag] photo_editor_render result ok=%1 textureId=%2 size=%3x%4 error=%5")
                                 .arg(ok)
                                 .arg(textureId)
                                 .arg(textureSize.width())
                                 .arg(textureSize.height())
                                 .arg(error ? *error : QString()));
        }
        if (ok) {
            logWorkerMessage(QStringLiteral("[diag] publishToSlot begin frame=%1 slot=%2 textureId=%3 size=%4x%5")
                                 .arg(frameIndex)
                                 .arg(renderSlot)
                                 .arg(textureId)
                                 .arg(textureSize.width())
                                 .arg(textureSize.height()));
            ok = publishBridge.publishToSlot(textureId, textureSize, renderSlot, error);
            logWorkerMessage(QStringLiteral("[diag] publishToSlot end frame=%1 slot=%2 ok=%3 error=%4")
                                 .arg(frameIndex)
                                 .arg(renderSlot)
                                 .arg(ok)
                                 .arg(error ? *error : QString()));
        }
        libraryContext->doneCurrent();

        if (!ok) {
            slotPool->abandonRenderSlot(renderSlot);
            publishActive = false;
            return false;
        }

        if (!slotPool->submitRenderedFrame(renderSlot, frameIndex, frame)) {
            slotPool->abandonRenderSlot(renderSlot);
            publishActive = false;
            return false;
        }

        session.renderReady = false;
        session.latestProgress = 100;
        publishActive = false;
        return true;
    }
};

D3D11NativeWorker::D3D11NativeWorker(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
}

D3D11NativeWorker::~D3D11NativeWorker()
{
    shutdown();
}

bool D3D11NativeWorker::initialize(D3D11NativeSlotPool *slotPool, QSize outputSize)
{
    if (m_initialized || slotPool == nullptr) {
        return false;
    }

    m_slotPool = slotPool;
    m_outputSize = sanitizedSize(outputSize);

    QString error;
    if (!m_impl->initialize(slotPool, m_outputSize, &error)) {
        emit initializationFailed(error);
        m_slotPool = nullptr;
        return false;
    }

    m_slotPool->reset();
    m_initialized = true;
    logWorkerMessage(QStringLiteral("Initialized. Worker now uses a non-shared GLES2 algorithm context plus a publish bridge to shared D3D11 textures."));
    return true;
}

void D3D11NativeWorker::setOutputSize(QSize size)
{
    const QSize safeSize = sanitizedSize(size);
    bool changed = false;
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_outputSize != safeSize) {
            m_outputSize = safeSize;
            changed = true;
        }
    }

    if (!changed || !m_initialized) {
        return;
    }

    QString error;
    if (!m_impl->updateOutputSize(safeSize, &error)) {
        emit initializationFailed(error);
        return;
    }

    scheduleRender(0);
}

void D3D11NativeWorker::setEffectParameters(const ImageEffectParameters &parameters)
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_effectParameters = parameters;
    }

    m_impl->session.latestParameters = parameters;
    m_impl->session.hasLatestParameters = true;
    if (m_impl->session.processInFlight || m_impl->session.renderReady || m_impl->publishActive) {
        m_impl->session.parametersDirty = true;
        return;
    }

    scheduleRender(0);
}

void D3D11NativeWorker::loadImageDirectory(const QString &directoryPath)
{
    if (!m_initialized) {
        emit imageDirectoryLoadFinished(false,
                                        QStringLiteral("D3D11 native worker is not initialized."),
                                        -1,
                                        0,
                                        {},
                                        {});
        return;
    }

    logWorkerMessage(QStringLiteral("Loading image directory: %1").arg(directoryPath));
    QString error;
    const bool loaded = m_impl->loadImageDirectory(directoryPath, currentOutputSize(), &error);
    if (!loaded) {
        logWorkerMessage(QStringLiteral("Image directory load failed: %1").arg(error));
        emit imageDirectoryLoadFinished(false, error, -1, 0, {}, {});
        return;
    }

    m_impl->session.latestParameters = currentEffectParameters();
    m_impl->session.hasLatestParameters = true;
    m_impl->session.parametersDirty = false;

    logWorkerMessage(QStringLiteral("Loaded %1 images. Current=%2 size=%3x%4")
                         .arg(m_impl->imagePaths.size())
                         .arg(m_impl->currentDisplayName())
                         .arg(m_impl->currentImage.width())
                         .arg(m_impl->currentImage.height()));
    emit imageDirectoryLoadFinished(true,
                                    {},
                                    m_impl->currentImageIndex,
                                    m_impl->imagePaths.size(),
                                    m_impl->currentDisplayName(),
                                    m_impl->currentImage.size());
    emitImageSelection();
    emit processingProgressChanged(0);
    scheduleRender(0);
}

void D3D11NativeWorker::selectNextImage()
{
    if (!m_initialized) {
        return;
    }

    QString error;
    if (!m_impl->selectRelativeImage(1, currentOutputSize(), &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
        return;
    }

    m_impl->session.latestParameters = currentEffectParameters();
    m_impl->session.hasLatestParameters = true;
    m_impl->session.parametersDirty = false;
    emitImageSelection();
    emit processingProgressChanged(0);
    scheduleRender(0);
}

void D3D11NativeWorker::selectPreviousImage()
{
    if (!m_initialized) {
        return;
    }

    QString error;
    if (!m_impl->selectRelativeImage(-1, currentOutputSize(), &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
        return;
    }

    m_impl->session.latestParameters = currentEffectParameters();
    m_impl->session.hasLatestParameters = true;
    m_impl->session.parametersDirty = false;
    emitImageSelection();
    emit processingProgressChanged(0);
    scheduleRender(0);
}

void D3D11NativeWorker::requestRender()
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_renderScheduled = false;
    }

    if (!m_initialized || m_slotPool == nullptr || !m_impl->hasImage()) {
        return;
    }

    logWorkerMessage(QStringLiteral("[diag] requestRender enter processInFlight=%1 renderReady=%2 publishActive=%3 frameIndex=%4 imageIndex=%5")
                         .arg(m_impl->session.processInFlight)
                         .arg(m_impl->session.renderReady)
                         .arg(m_impl->publishActive)
                         .arg(m_frameIndex)
                         .arg(m_impl->currentImageIndex));

    QString error;
    if (m_impl->session.renderReady && !m_impl->publishActive) {
        QElapsedTimer timer;
        timer.start();
        D3D11NativeFrame frame;
        const bool published = m_impl->renderAndPublish(currentOutputSize(), m_slotPool, m_frameIndex, &frame, &error);
        if (!published) {
            if (error.isEmpty()) {
                scheduleRender(4);
                return;
            }
            emit initializationFailed(error);
            return;
        }

        logWorkerMessage(QStringLiteral("Published frame=%1 slot=%2 output=%3x%4 elapsedMs=%5")
                             .arg(m_frameIndex)
                             .arg(frame.slotIndex)
                             .arg(frame.size.width())
                             .arg(frame.size.height())
                             .arg(QString::number(double(timer.nsecsElapsed()) / 1000000.0, 'f', 2)));
        emit frameReady(frame.slotIndex, frame.generation, frame.size, frame.frameIndex);
        ++m_frameIndex;

        if (m_impl->session.parametersDirty) {
            m_impl->session.parametersDirty = false;
            scheduleRender(0);
        }
        return;
    }

    if (m_impl->session.processInFlight || m_impl->publishActive) {
        return;
    }

    if (!m_impl->session.hasLatestParameters) {
        m_impl->session.latestParameters = currentEffectParameters();
        m_impl->session.hasLatestParameters = true;
    }

    if (!m_impl->startProcess(m_impl->session.latestParameters,
                              currentOutputSize(),
                              this,
                              this,
                              &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
    }
}

void D3D11NativeWorker::shutdown()
{
    m_initialized = false;
    m_renderScheduled = false;
    m_frameIndex = 0;

    if (m_impl) {
        m_impl->shutdownRequested = true;
        m_impl->destroySession();
    }

    m_slotPool = nullptr;
    m_impl = std::make_unique<Impl>();
}

void D3D11NativeWorker::emitImageSelection()
{
    if (!m_impl->hasImage()) {
        emit imageSelectionChanged(-1, 0, {}, {});
        return;
    }

    emit imageSelectionChanged(m_impl->currentImageIndex,
                               m_impl->imagePaths.size(),
                               m_impl->currentDisplayName(),
                               m_impl->currentImage.size());
}

QSize D3D11NativeWorker::currentOutputSize() const
{
    QMutexLocker locker(&m_stateMutex);
    return sanitizedSize(m_outputSize);
}

ImageEffectParameters D3D11NativeWorker::currentEffectParameters() const
{
    QMutexLocker locker(&m_stateMutex);
    return m_effectParameters;
}

void D3D11NativeWorker::scheduleRender(int delayMs)
{
    QMutexLocker locker(&m_stateMutex);
    if (m_renderScheduled) {
        return;
    }

    m_renderScheduled = true;
    QTimer::singleShot(qMax(0, delayMs), this, &D3D11NativeWorker::requestRender);
}

void D3D11NativeWorker::onProcessProgressEvent(int progress, bool isEnd)
{
    if (!m_initialized || m_impl->shutdownRequested) {
        return;
    }

    logWorkerMessage(QStringLiteral("[diag] onProcessProgressEvent progress=%1 isEnd=%2 processInFlight(before)=%3 renderReady(before)=%4")
                         .arg(progress)
                         .arg(isEnd)
                         .arg(m_impl->session.processInFlight)
                         .arg(m_impl->session.renderReady));

    m_impl->session.latestProgress = progress;
    emit processingProgressChanged(progress);

    if (!isEnd) {
        return;
    }

    m_impl->session.processInFlight = false;
    m_impl->session.renderReady = true;
    logWorkerMessage(QStringLiteral("[diag] onProcessProgressEvent completed processInFlight(after)=%1 renderReady(after)=%2")
                         .arg(m_impl->session.processInFlight)
                         .arg(m_impl->session.renderReady));
    scheduleRender(0);
}
