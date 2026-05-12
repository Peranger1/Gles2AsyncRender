#include "photo_editor_async_render_facade.h"

#include "adapters/photo_editor/photo_editor_render_payload.h"
#include "adapters/photo_editor/photo_editor_render_session.h"
#include "framework/backend/win_angle_d3d11/angle_standalone_runtime.h"
#include "framework/backend/win_angle_d3d11/d3d11_frame_publisher.h"
#include "framework/core/async_render_worker.h"
#include "framework/core/shared_frame_slot_pool.h"
#include "runtime_diagnostics.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMutexLocker>

namespace
{
void logFacadeMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[PhotoEditorAsyncRenderFacade]", message);
}

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

QString currentDisplayName(const QStringList &imagePaths, int currentImageIndex)
{
    if (currentImageIndex < 0 || currentImageIndex >= imagePaths.size()) {
        return {};
    }
    return QFileInfo(imagePaths[currentImageIndex]).fileName();
}
}

struct PhotoEditorAsyncRenderFacade::ImageCatalogState final
{
    QStringList imagePaths;
    int currentImageIndex = -1;
    QImage currentImage;
    QString currentImagePath;

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

    bool hasImage() const
    {
        return !currentImage.isNull();
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
};

PhotoEditorAsyncRenderFacade::PhotoEditorAsyncRenderFacade(QObject *parent)
    : QObject(parent)
    , m_catalog(std::make_unique<ImageCatalogState>())
    , m_worker(new AsyncRenderWorker(this))
{
    connect(m_worker, &AsyncRenderWorker::frameReady,
            this, &PhotoEditorAsyncRenderFacade::frameReady);
    connect(m_worker, &AsyncRenderWorker::workerFailed,
            this, &PhotoEditorAsyncRenderFacade::initializationFailed);
    connect(m_worker, &AsyncRenderWorker::processingProgressChanged,
            this, &PhotoEditorAsyncRenderFacade::processingProgressChanged);
}

PhotoEditorAsyncRenderFacade::~PhotoEditorAsyncRenderFacade()
{
    shutdown();
}

bool PhotoEditorAsyncRenderFacade::initialize(ISharedFrameSlotPool *slotPool, QSize outputSize)
{
    if (m_initialized || slotPool == nullptr) {
        return false;
    }

    m_slotPool = slotPool;
    m_outputSize = sanitizedSize(outputSize);

    QString error;
    if (!m_worker->configure(std::make_unique<AngleStandaloneRuntime>(),
                             std::make_unique<PhotoEditorRenderSession>(),
                             std::make_unique<D3D11FramePublisher>(),
                             &error)
        || !m_worker->initialize(slotPool)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
        m_slotPool = nullptr;
        return false;
    }

    m_initialized = true;
    m_shuttingDown = false;
    m_requestSequence = 0;
    logFacadeMessage(QStringLiteral("Initialized. App facade now forwards requests to a generic async render worker."));
    return true;
}

void PhotoEditorAsyncRenderFacade::setOutputSize(QSize size)
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

    if (changed && m_initialized) {
        submitLatestRequest();
    }
}

void PhotoEditorAsyncRenderFacade::setEffectParameters(const ImageEffectParameters &parameters)
{
    if (m_shuttingDown) {
        return;
    }

    {
        QMutexLocker locker(&m_stateMutex);
        m_effectParameters = parameters;
    }

    if (m_initialized && m_catalog->hasImage()) {
        submitLatestRequest();
    }
}

void PhotoEditorAsyncRenderFacade::loadImageDirectory(const QString &directoryPath)
{
    if (m_shuttingDown) {
        return;
    }

    if (!m_initialized) {
        emit imageDirectoryLoadFinished(false,
                                        QStringLiteral("Photo editor async render facade is not initialized."),
                                        -1,
                                        0,
                                        {},
                                        {});
        return;
    }

    logFacadeMessage(QStringLiteral("Loading image directory: %1").arg(directoryPath));
    QString error;
    const bool loaded = m_catalog->loadImageDirectory(directoryPath, &error);
    if (!loaded) {
        logFacadeMessage(QStringLiteral("Image directory load failed: %1").arg(error));
        emit imageDirectoryLoadFinished(false, error, -1, 0, {}, {});
        return;
    }

    logFacadeMessage(QStringLiteral("Loaded %1 images. Current=%2 size=%3x%4")
                         .arg(m_catalog->imagePaths.size())
                         .arg(currentDisplayName(m_catalog->imagePaths, m_catalog->currentImageIndex))
                         .arg(m_catalog->currentImage.width())
                         .arg(m_catalog->currentImage.height()));
    emit imageDirectoryLoadFinished(true,
                                    {},
                                    m_catalog->currentImageIndex,
                                    m_catalog->imagePaths.size(),
                                    currentDisplayName(m_catalog->imagePaths, m_catalog->currentImageIndex),
                                    m_catalog->currentImage.size());
    emitImageSelection();
    emit processingProgressChanged(0);
    submitLatestRequest();
}

void PhotoEditorAsyncRenderFacade::selectNextImage()
{
    if (!m_initialized || m_shuttingDown) {
        return;
    }

    QString error;
    if (!m_catalog->selectRelativeImage(1, &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
        return;
    }

    emitImageSelection();
    emit processingProgressChanged(0);
    submitLatestRequest();
}

void PhotoEditorAsyncRenderFacade::selectPreviousImage()
{
    if (!m_initialized || m_shuttingDown) {
        return;
    }

    QString error;
    if (!m_catalog->selectRelativeImage(-1, &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
        return;
    }

    emitImageSelection();
    emit processingProgressChanged(0);
    submitLatestRequest();
}

void PhotoEditorAsyncRenderFacade::requestRender()
{
    if (!m_initialized || m_shuttingDown || !m_catalog->hasImage()) {
        return;
    }

    m_worker->requestRender();
}

void PhotoEditorAsyncRenderFacade::onSlotAvailable()
{
    if (m_worker != nullptr) {
        m_worker->onSlotAvailable();
    }
}

void PhotoEditorAsyncRenderFacade::shutdown()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;
    m_initialized = false;
    m_requestSequence = 0;

    if (m_worker) {
        m_worker->shutdown();
    }

    m_slotPool = nullptr;
    m_catalog = std::make_unique<ImageCatalogState>();
}

void PhotoEditorAsyncRenderFacade::emitImageSelection()
{
    if (!m_catalog->hasImage()) {
        emit imageSelectionChanged(-1, 0, {}, {});
        return;
    }

    emit imageSelectionChanged(m_catalog->currentImageIndex,
                               m_catalog->imagePaths.size(),
                               currentDisplayName(m_catalog->imagePaths, m_catalog->currentImageIndex),
                               m_catalog->currentImage.size());
}

void PhotoEditorAsyncRenderFacade::submitLatestRequest()
{
    if (!m_initialized || !m_catalog->hasImage()) {
        return;
    }

    m_worker->submitLatestRequest(buildLatestRequest());
}

AsyncRenderRequest PhotoEditorAsyncRenderFacade::buildLatestRequest() const
{
    QMutexLocker locker(&m_stateMutex);

    AsyncRenderRequest request;
    request.sequence = ++m_requestSequence;
    request.outputSize = m_outputSize;

    auto payload = std::make_shared<PhotoEditorRenderPayload>();
    payload->sourceKey = m_catalog->currentImagePath;
    payload->sourceImageCacheKey = m_catalog->currentImage.cacheKey();
    payload->sourceImage = m_catalog->currentImage;
    payload->parameters = m_effectParameters;
    request.payload = payload;
    return request;
}
