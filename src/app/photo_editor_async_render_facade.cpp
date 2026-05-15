#include "photo_editor_async_render_facade.h"

#include "adapters/photo_editor/photo_editor_cpu_preview_processor.h"
#include "adapters/photo_editor/photo_editor_cpu_preview_payload.h"
#include "adapters/photo_editor/photo_editor_render_payload.h"
#include "adapters/photo_editor/photo_editor_work_processor.h"
#include "async_task_facade.h"
#include "framework/backend/platform_render_backend.h"
#include "framework/core/noop_work_runtime.h"
#include "runtime_diagnostics.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMutexLocker>

#include <variant>

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
{
}

PhotoEditorAsyncRenderFacade::~PhotoEditorAsyncRenderFacade()
{
    shutdown();
}

bool PhotoEditorAsyncRenderFacade::initialize(IPlatformRenderBackend *backend, QSize outputSize)
{
    if (m_initialized || backend == nullptr || backend->frameWriter() == nullptr) {
        return false;
    }

    m_backend = backend;
    m_outputSize = sanitizedSize(outputSize);
    m_taskFacade = std::make_unique<AsyncTaskFacade>();
    m_cpuPreviewTaskFacade = std::make_unique<AsyncTaskFacade>();
    connect(m_taskFacade.get(), &AsyncTaskFacade::stateChanged,
            this, &PhotoEditorAsyncRenderFacade::handleStateChanged);
    connect(m_taskFacade.get(), &AsyncTaskFacade::progressChanged,
            this, &PhotoEditorAsyncRenderFacade::handleProgress);
    connect(m_taskFacade.get(), &AsyncTaskFacade::messageEmitted,
            this, &PhotoEditorAsyncRenderFacade::handleMessage);
    connect(m_taskFacade.get(), &AsyncTaskFacade::resultReady,
            this, &PhotoEditorAsyncRenderFacade::handleJobResult);
    connect(m_taskFacade.get(), &AsyncTaskFacade::fatalError,
            this, &PhotoEditorAsyncRenderFacade::handlePipelineError);
    connect(m_cpuPreviewTaskFacade.get(), &AsyncTaskFacade::stateChanged,
            this, &PhotoEditorAsyncRenderFacade::handleStateChanged);
    connect(m_cpuPreviewTaskFacade.get(), &AsyncTaskFacade::progressChanged,
            this, &PhotoEditorAsyncRenderFacade::handleProgress);
    connect(m_cpuPreviewTaskFacade.get(), &AsyncTaskFacade::messageEmitted,
            this, &PhotoEditorAsyncRenderFacade::handleMessage);
    connect(m_cpuPreviewTaskFacade.get(), &AsyncTaskFacade::resultReady,
            this, &PhotoEditorAsyncRenderFacade::handleJobResult);
    connect(m_cpuPreviewTaskFacade.get(), &AsyncTaskFacade::fatalError,
            this, &PhotoEditorAsyncRenderFacade::handlePipelineError);

    QString error;
    if (!m_taskFacade->initialize(backend,
                                  std::make_unique<PhotoEditorWorkProcessor>(),
                                  m_outputSize,
                                  &error)) {
        handlePipelineError(error);
        shutdown();
        return false;
    }
    if (!m_cpuPreviewTaskFacade->initializeWithoutBackend(std::make_unique<NoopWorkRuntime>(),
                                                          std::make_unique<PhotoEditorCpuPreviewProcessor>(),
                                                          &error)) {
        handlePipelineError(error);
        shutdown();
        return false;
    }

    m_initialized = true;
    m_shuttingDown = false;
    m_requestSequence = 0;
    logFacadeMessage(QStringLiteral("Initialized. App facade now forwards requests to the generic async pipeline."));
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
        if (m_taskFacade) {
            m_taskFacade->setOutputSize(safeSize);
        }
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
    if (!m_initialized || m_shuttingDown || !m_catalog->hasImage() || !m_taskFacade) {
        return;
    }

    m_taskFacade->requestPump();
}

void PhotoEditorAsyncRenderFacade::requestCpuPreview()
{
    if (!m_initialized || m_shuttingDown || !m_catalog->hasImage() || !m_cpuPreviewTaskFacade) {
        return;
    }

    submitCpuPreviewRequest();
}

void PhotoEditorAsyncRenderFacade::onPublicationCapacityAvailable()
{
    if (m_taskFacade) {
        m_taskFacade->onPublicationCapacityAvailable();
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

    if (m_taskFacade) {
        m_taskFacade->shutdown();
    }
    if (m_cpuPreviewTaskFacade) {
        m_cpuPreviewTaskFacade->shutdown();
    }

    m_taskFacade.reset();
    m_cpuPreviewTaskFacade.reset();
    m_backend = nullptr;
    m_catalog = std::make_unique<ImageCatalogState>();
}

void PhotoEditorAsyncRenderFacade::handleStateChanged(RequestId requestId, WorkState state)
{
    Q_UNUSED(requestId);
    Q_UNUSED(state);
}

void PhotoEditorAsyncRenderFacade::handleProgress(RequestId requestId, int progress, bool isFinal)
{
    Q_UNUSED(requestId);
    Q_UNUSED(isFinal);
    if (!m_shuttingDown) {
        emit processingProgressChanged(progress);
    }
}

void PhotoEditorAsyncRenderFacade::handleMessage(RequestId requestId, const QString &message)
{
    Q_UNUSED(requestId);
    if (!m_shuttingDown && !message.isEmpty()) {
        logFacadeMessage(QStringLiteral("Pipeline message: %1").arg(message));
    }
}

void PhotoEditorAsyncRenderFacade::handleJobResult(const JobResult &result)
{
    if (m_shuttingDown) {
        return;
    }

    emit jobResultReady(result);

    if (std::holds_alternative<FrameTicket>(result.payload)) {
        emit frameReady(std::get<FrameTicket>(result.payload));
        return;
    }

    if (std::holds_alternative<CpuImageResult>(result.payload)) {
        const CpuImageResult &previewResult = std::get<CpuImageResult>(result.payload);
        const QString description = previewResult.metadata.value(QStringLiteral("description")).toString();
        emit cpuPreviewReady(previewResult.image, description);
        return;
    }

    logFacadeMessage(QStringLiteral("Ignoring non-frame job result. kind=%1 requestId=%2")
                         .arg(result.resultKind)
                         .arg(result.requestId));
}

void PhotoEditorAsyncRenderFacade::handlePipelineError(const QString &reason)
{
    if (m_shuttingDown || reason.isEmpty()) {
        return;
    }

    logFacadeMessage(QStringLiteral("Pipeline error: %1").arg(reason));
    emit initializationFailed(reason);
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
    if (!m_initialized || !m_catalog->hasImage() || !m_taskFacade) {
        return;
    }

    m_taskFacade->submit(buildLatestWork());
}

void PhotoEditorAsyncRenderFacade::submitCpuPreviewRequest()
{
    if (!m_initialized || !m_catalog->hasImage() || !m_cpuPreviewTaskFacade) {
        return;
    }

    m_cpuPreviewTaskFacade->submit(buildCpuPreviewWork());
}

WorkEnvelope PhotoEditorAsyncRenderFacade::buildLatestWork() const
{
    QMutexLocker locker(&m_stateMutex);

    WorkEnvelope work;
    work.requestId = ++m_requestSequence;
    work.version = work.requestId;
    work.laneId = QStringLiteral("photo_editor.preview");
    work.mergeKey = work.laneId;
    work.requestKind = QStringLiteral("photo_editor.process");
    work.hints.deliveryPolicy = ResultDeliveryPolicy::AlwaysDeliver;

    auto payload = std::make_shared<PhotoEditorRenderPayload>();
    payload->sourceKey = m_catalog->currentImagePath;
    payload->sourceImageCacheKey = m_catalog->currentImage.cacheKey();
    payload->sourceImage = m_catalog->currentImage;
    payload->parameters = m_effectParameters;
    payload->outputSize = m_outputSize;
    work.payload = payload;
    return work;
}

WorkEnvelope PhotoEditorAsyncRenderFacade::buildCpuPreviewWork() const
{
    QMutexLocker locker(&m_stateMutex);

    WorkEnvelope work;
    work.requestId = ++m_requestSequence;
    work.version = work.requestId;
    work.laneId = QStringLiteral("photo_editor.cpu_preview");
    work.mergeKey = work.laneId;
    work.requestKind = QStringLiteral("photo_editor.cpu_preview");
    work.hints.deliveryPolicy = ResultDeliveryPolicy::AlwaysDeliver;

    auto payload = std::make_shared<PhotoEditorCpuPreviewPayload>();
    payload->sourceKey = m_catalog->currentImagePath;
    payload->sourceImageCacheKey = m_catalog->currentImage.cacheKey();
    payload->sourceImage = m_catalog->currentImage;
    payload->parameters = m_effectParameters;
    payload->previewSize = m_outputSize.boundedTo(QSize(320, 320));
    work.payload = payload;
    return work;
}
