#include "d3d11_native_worker.h"

#include "adapters/photo_editor/photo_editor_render_payload.h"
#include "adapters/photo_editor/photo_editor_render_session.h"
#include "framework/backend/win_angle_d3d11/angle_standalone_runtime.h"
#include "framework/backend/win_angle_d3d11/d3d11_frame_publisher.h"
#include "framework/core/async_render_executor.h"
#include "framework/core/shared_frame_slot_pool.h"
#include "runtime_diagnostics.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>

namespace
{
void logWorkerMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[D3D11NativeWorker]", message);
}

void logWorkerDiag(const QString &message)
{
    RuntimeDiagnostics::logDiag("[D3D11NativeWorker]", message);
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
}

struct D3D11NativeWorker::Impl final
{
    std::unique_ptr<AngleStandaloneRuntime> algorithmRuntime;
    PhotoEditorRenderSession session;
    D3D11FramePublisher publishBridge;
    AsyncRenderExecutor executor;

    QStringList imagePaths;
    int currentImageIndex = -1;
    QImage currentImage;
    QString currentImagePath;
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

    bool initialize(ISharedFrameSlotPool *slotPool, QString *error)
    {
        algorithmRuntime = std::make_unique<AngleStandaloneRuntime>();
        if (!algorithmRuntime->initialize(error)) {
            return false;
        }

        if (!session.initialize(*algorithmRuntime, error)) {
            return false;
        }

        if (!algorithmRuntime->makeCurrent(error)) {
            return false;
        }
        const bool publishReady = publishBridge.initialize(algorithmRuntime.get(), slotPool, error);
        algorithmRuntime->doneCurrent(nullptr);
        if (!publishReady) {
            return false;
        }

        if (!executor.initialize(algorithmRuntime.get(), &session, &publishBridge, slotPool, error)) {
            return false;
        }

        const AngleStandaloneRuntime::RendererIdentity identity = algorithmRuntime->queryRendererIdentity();
        logWorkerMessage(QStringLiteral(
                             "Worker standalone runtime ready.\n"
                             "  EGL module=%1 (%2)\n"
                             "  GLES module=%3 (%4)\n"
                             "  EGLDisplay=%5\n"
                             "  EGLDevice=%6\n"
                             "  D3D11Device=%7\n"
                             "  AdapterLuid=%8\n"
                             "  GL vendor=%9 renderer=%10 version=%11")
                             .arg(quintptr(identity.eglModule), 0, 16)
                             .arg(identity.eglModulePath)
                             .arg(quintptr(identity.glesModule), 0, 16)
                             .arg(identity.glesModulePath)
                             .arg(quintptr(identity.eglDisplay), 0, 16)
                             .arg(quintptr(identity.eglDevice), 0, 16)
                             .arg(quintptr(identity.d3d11Device), 0, 16)
                             .arg(identity.adapterLuid)
                             .arg(identity.glVendor)
                             .arg(identity.glRenderer)
                             .arg(identity.glVersion));
        return true;
    }

    bool loadImageDirectory(const QString &directoryPath, QString *error)
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
        currentImagePath = paths.first();
        currentImage = QImage(currentImagePath);
        if (currentImage.isNull()) {
            if (error) {
                *error = QStringLiteral("Failed to load the first image from the selected directory.");
            }
            return false;
        }
        return true;
    }

    bool selectRelativeImage(int delta, QString *error)
    {
        if (imagePaths.isEmpty()) {
            return false;
        }

        const int count = imagePaths.size();
        currentImageIndex = (currentImageIndex + delta + count) % count;
        currentImagePath = imagePaths[currentImageIndex];
        currentImage = QImage(currentImagePath);
        if (currentImage.isNull()) {
            if (error) {
                *error = QStringLiteral("Failed to load image: %1").arg(currentImagePath);
            }
            return false;
        }
        return true;
    }

    AsyncRenderRequest makeLatestRequest(const QSize &outputSize,
                                         const ImageEffectParameters &parameters,
                                         quint64 sequence) const
    {
        AsyncRenderRequest request;
        request.sequence = sequence;
        request.outputSize = sanitizedSize(outputSize);

        auto payload = std::make_shared<PhotoEditorRenderPayload>();
        payload->sourceKey = currentImagePath;
        payload->sourceImageCacheKey = currentImage.cacheKey();
        payload->sourceImage = currentImage;
        payload->parameters = parameters;
        request.payload = payload;
        return request;
    }

    void shutdownRuntime()
    {
        session.shutdown();
        executor.shutdown();

        if (!algorithmRuntime) {
            return;
        }

        if (algorithmRuntime->makeCurrent(nullptr)) {
            publishBridge.releaseGlResources();
            algorithmRuntime->doneCurrent(nullptr);
        }
        algorithmRuntime->shutdown();
        algorithmRuntime.reset();
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

bool D3D11NativeWorker::initialize(ISharedFrameSlotPool *slotPool, QSize outputSize)
{
    if (m_initialized || slotPool == nullptr) {
        return false;
    }

    m_slotPool = slotPool;
    m_outputSize = sanitizedSize(outputSize);

    QString error;
    if (!m_impl->initialize(slotPool, &error)) {
        emit initializationFailed(error);
        m_slotPool = nullptr;
        return false;
    }

    m_slotPool->reset();
    m_initialized = true;
    m_shuttingDown = false;
    m_waitingForFreeSlot = false;
    m_requestSequence = 0;
    logWorkerMessage(QStringLiteral("Initialized. Worker now uses a standalone ANGLE runtime, a photo editor render session, and a standalone publish bridge to shared D3D11 textures."));
    return true;
}

void D3D11NativeWorker::setOutputSize(QSize size)
{
    if (m_shuttingDown) {
        return;
    }

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

    enqueueLatestRenderRequest();
    schedulePump(0);
}

void D3D11NativeWorker::setEffectParameters(const ImageEffectParameters &parameters)
{
    if (m_shuttingDown) {
        return;
    }

    {
        QMutexLocker locker(&m_stateMutex);
        m_effectParameters = parameters;
    }

    if (!m_initialized || !m_impl->hasImage()) {
        return;
    }

    enqueueLatestRenderRequest();
    schedulePump(0);
}

void D3D11NativeWorker::loadImageDirectory(const QString &directoryPath)
{
    if (m_shuttingDown) {
        return;
    }

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
    const bool loaded = m_impl->loadImageDirectory(directoryPath, &error);
    if (!loaded) {
        logWorkerMessage(QStringLiteral("Image directory load failed: %1").arg(error));
        emit imageDirectoryLoadFinished(false, error, -1, 0, {}, {});
        return;
    }

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
    enqueueLatestRenderRequest();
    schedulePump(0);
}

void D3D11NativeWorker::selectNextImage()
{
    if (!m_initialized || m_shuttingDown) {
        return;
    }

    QString error;
    if (!m_impl->selectRelativeImage(1, &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
        return;
    }

    emitImageSelection();
    emit processingProgressChanged(0);
    enqueueLatestRenderRequest();
    schedulePump(0);
}

void D3D11NativeWorker::selectPreviousImage()
{
    if (!m_initialized || m_shuttingDown) {
        return;
    }

    QString error;
    if (!m_impl->selectRelativeImage(-1, &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
        return;
    }

    emitImageSelection();
    emit processingProgressChanged(0);
    enqueueLatestRenderRequest();
    schedulePump(0);
}

void D3D11NativeWorker::requestRender()
{
    pumpRender();
}

void D3D11NativeWorker::shutdown()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;
    m_initialized = false;
    m_renderScheduled = false;
    m_waitingForFreeSlot = false;
    m_frameIndex = 0;
    m_requestSequence = 0;

    if (m_impl) {
        m_impl->shutdownRequested = true;
        m_impl->shutdownRuntime();
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

void D3D11NativeWorker::enqueueLatestRenderRequest()
{
    if (!m_initialized || !m_impl->hasImage()) {
        return;
    }

    const quint64 nextSequence = ++m_requestSequence;
    m_impl->executor.updateLatestRequest(
        m_impl->makeLatestRequest(currentOutputSize(), currentEffectParameters(), nextSequence));
}

void D3D11NativeWorker::schedulePump(int delayMs)
{
    QMutexLocker locker(&m_stateMutex);
    if (m_renderScheduled || m_shuttingDown || (m_impl && m_impl->shutdownRequested)) {
        return;
    }

    m_renderScheduled = true;
    QTimer::singleShot(qMax(0, delayMs), this, &D3D11NativeWorker::pumpRender);
}

void D3D11NativeWorker::pumpRender()
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_renderScheduled = false;
    }

    if (m_shuttingDown || (m_impl && m_impl->shutdownRequested)) {
        logWorkerDiag(QStringLiteral("[diag] pumpRender skipped because shutdown is in progress"));
        return;
    }

    if (!m_initialized || m_slotPool == nullptr || !m_impl->hasImage()) {
        return;
    }

    QString error;
    if (m_impl->session.hasRenderReady()) {
        QElapsedTimer timer;
        timer.start();
        PublishedFrame frame;
        if (!m_impl->executor.tryPublishReadyFrame(m_frameIndex, &frame, &error)) {
            if (error.isEmpty()) {
                m_waitingForFreeSlot = m_impl->executor.isWaitingForSlot();
                return;
            }
            emit initializationFailed(error);
            return;
        }

        m_waitingForFreeSlot = false;
        logWorkerMessage(QStringLiteral("Published frame=%1 slot=%2 output=%3x%4 elapsedMs=%5")
                             .arg(m_frameIndex)
                             .arg(frame.slotIndex)
                             .arg(frame.size.width())
                             .arg(frame.size.height())
                             .arg(QString::number(double(timer.nsecsElapsed()) / 1000000.0, 'f', 2)));
        emit frameReady(frame.slotIndex, frame.generation, frame.size, frame.frameIndex);
        ++m_frameIndex;

        if (m_impl->executor.hasPendingRequest()
            && m_impl->executor.currentRequestSequence() > m_impl->session.requestSequence()) {
            schedulePump(0);
        }
        return;
    }

    if (m_impl->session.isProcessInFlight()) {
        return;
    }

    if (!m_impl->executor.hasPendingRequest()) {
        enqueueLatestRenderRequest();
    }

    if (!m_impl->executor.tryStartProcess(this, &processProgressThunk, this, &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
    }
}

void D3D11NativeWorker::onSlotAvailableForWorker()
{
    if (!m_initialized || m_shuttingDown || !m_waitingForFreeSlot) {
        return;
    }

    m_waitingForFreeSlot = false;
    schedulePump(0);
}

void D3D11NativeWorker::onProcessProgressEvent(int progress, bool isEnd)
{
    if (!m_initialized || m_shuttingDown || m_impl->shutdownRequested) {
        return;
    }

    m_impl->session.handleProgressEvent(progress, isEnd);
    emit processingProgressChanged(progress);

    if (!isEnd) {
        return;
    }

    schedulePump(0);
}
