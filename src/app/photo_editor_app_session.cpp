#include "photo_editor_app_session.h"

#include "framework/execution/request_channel.h"
#include "framework/platform/platform_backend.h"
#include "framework/platform/runtime.h"
#include "framework/platform/writer.h"
#include "photo_editor_demo_handlers.h"
#include "photo_editor_demo_requests.h"
#include "runtime_diagnostics.h"

#include <QDir>
#include <QFileInfo>

namespace
{
constexpr const char *kGpuPreviewTypeId = "photo_editor.preview.gpu.async";
constexpr const char *kCpuPreviewTypeId = "photo_editor.preview.cpu.sync";

void logSessionMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[PhotoEditorAppSession]", message);
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

struct PhotoEditorAppSession::ImageCatalogState final
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

PhotoEditorAppSession::PhotoEditorAppSession(QObject *parent)
    : QObject(parent)
    , m_catalog(std::make_unique<ImageCatalogState>())
{
}

PhotoEditorAppSession::~PhotoEditorAppSession()
{
    shutdown();
}

bool PhotoEditorAppSession::initialize(IPlatformBackend *backend, QSize outputSize)
{
    if (m_initialized || backend == nullptr) {
        return false;
    }

    QString error;
    std::unique_ptr<IRuntime> runtime = backend->createRuntime();
    if (!runtime || !runtime->initialize(&error)) {
        emit initializationFailed(error.isEmpty()
                                      ? QStringLiteral("Failed to initialize the platform runtime.")
                                      : error);
        return false;
    }

    auto dispatcher = std::make_unique<RequestDispatcher>();
    dispatcher->registerChannel(std::make_unique<RequestChannel>(
        runtime.get(),
        std::make_unique<PhotoEditorGpuPreviewHandler>(),
        this));
    dispatcher->registerChannel(std::make_unique<RequestChannel>(
        nullptr,
        std::make_unique<PhotoEditorCpuPreviewHandler>(),
        this));

    m_backend = backend;
    m_runtime = std::move(runtime);
    m_dispatcher = std::move(dispatcher);
    m_outputSize = sanitizedSize(outputSize);
    m_requestSequence = 0;
    m_initialized = true;
    m_shuttingDown = false;
    m_hasPendingGpuPublish = false;
    logSessionMessage(QStringLiteral("Initialized. App session now owns the request dispatcher and the runtime."));
    return true;
}

void PhotoEditorAppSession::setOutputSize(QSize size)
{
    if (m_shuttingDown) {
        return;
    }

    const QSize safeSize = sanitizedSize(size);
    if (m_outputSize == safeSize) {
        return;
    }

    m_outputSize = safeSize;
    if (m_initialized && m_catalog->hasImage()) {
        submitGpuRequest();
    }
}

void PhotoEditorAppSession::setEffectParameters(const ImageEffectParameters &parameters)
{
    if (m_shuttingDown) {
        return;
    }

    m_effectParameters = parameters;
}

void PhotoEditorAppSession::loadImageDirectory(const QString &directoryPath)
{
    if (m_shuttingDown || !m_initialized) {
        emit imageDirectoryLoadFinished(false,
                                        QStringLiteral("Photo editor app session is not initialized."),
                                        -1,
                                        0,
                                        {},
                                        {});
        return;
    }

    QString error;
    const bool loaded = m_catalog->loadImageDirectory(directoryPath, &error);
    if (!loaded) {
        emit imageDirectoryLoadFinished(false, error, -1, 0, {}, {});
        return;
    }

    emit imageDirectoryLoadFinished(true,
                                    {},
                                    m_catalog->currentImageIndex,
                                    m_catalog->imagePaths.size(),
                                    currentDisplayName(m_catalog->imagePaths, m_catalog->currentImageIndex),
                                    m_catalog->currentImage.size());
    emitImageSelection();
    submitGpuRequest();
}

void PhotoEditorAppSession::selectNextImage()
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
    submitGpuRequest();
}

void PhotoEditorAppSession::selectPreviousImage()
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
    submitGpuRequest();
}

void PhotoEditorAppSession::requestRender()
{
    if (m_initialized && !m_shuttingDown && m_catalog->hasImage()) {
        submitGpuRequest();
    }
}

void PhotoEditorAppSession::requestCpuPreview()
{
    if (m_initialized && !m_shuttingDown && m_catalog->hasImage()) {
        submitCpuRequest();
    }
}

void PhotoEditorAppSession::onTextureConsumed()
{
    if (m_shuttingDown || !m_hasPendingGpuPublish) {
        return;
    }

    if (tryPublishGpuResult(m_pendingGpuPublish, m_runtime.get()) == GpuPublishDisposition::Published) {
        m_hasPendingGpuPublish = false;
        m_pendingGpuPublish = {};
    }
}

void PhotoEditorAppSession::shutdown()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;
    m_initialized = false;
    m_hasPendingGpuPublish = false;
    m_pendingGpuPublish = {};

    if (m_dispatcher) {
        m_dispatcher->shutdown();
    }
    if (m_backend && m_backend->writer()) {
        m_backend->writer()->reset();
    }
    if (m_runtime) {
        m_runtime->shutdown();
    }

    m_dispatcher.reset();
    m_runtime.reset();
    m_backend = nullptr;
    m_catalog = std::make_unique<ImageCatalogState>();
    m_requestSequence = 0;
}

void PhotoEditorAppSession::onResultReady(const ExecutionResult &result, IExecutionContext &context)
{
    if (m_shuttingDown || m_backend == nullptr) {
        return;
    }

    if (std::holds_alternative<RawGpuTextureResult>(result.payload)) {
        const RawGpuTextureResult &gpuResult = std::get<RawGpuTextureResult>(result.payload);
        const GpuPublishDisposition disposition = tryPublishGpuResult(gpuResult, context.runtime());
        if (disposition == GpuPublishDisposition::RetryLater) {
            m_pendingGpuPublish = gpuResult;
            m_hasPendingGpuPublish = true;
        }
        return;
    }

    if (std::holds_alternative<CpuImageResult>(result.payload)) {
        const CpuImageResult &previewResult = std::get<CpuImageResult>(result.payload);
        const QString description = previewResult.metadata.value(QStringLiteral("description")).toString();
        emit cpuPreviewReady(previewResult.image, description);
    }
}

void PhotoEditorAppSession::onRequestFailed(RequestId requestId, const QString &error)
{
    Q_UNUSED(requestId);
    if (!m_shuttingDown && !error.isEmpty()) {
        emit initializationFailed(error);
    }
}

void PhotoEditorAppSession::emitImageSelection()
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

void PhotoEditorAppSession::submitGpuRequest()
{
    if (m_dispatcher) {
        m_dispatcher->submit(buildGpuRequest());
    }
}

void PhotoEditorAppSession::submitCpuRequest()
{
    if (m_dispatcher) {
        m_dispatcher->submit(buildCpuRequest());
    }
}

PhotoEditorAppSession::GpuPublishDisposition PhotoEditorAppSession::tryPublishGpuResult(const RawGpuTextureResult &gpuResult,
                                                                                        IRuntime *sourceRuntime)
{
    if (m_backend == nullptr || m_backend->writer() == nullptr) {
        return GpuPublishDisposition::Failed;
    }

    if (sourceRuntime == nullptr) {
        emit initializationFailed(QStringLiteral("The GPU result cannot be published because the execution context runtime is unavailable."));
        return GpuPublishDisposition::Failed;
    }

    if (m_runtime && sourceRuntime != m_runtime.get()) {
        emit initializationFailed(QStringLiteral("The GPU result runtime does not match the app session runtime."));
        return GpuPublishDisposition::Failed;
    }

    QString error;
    TextureTicket ticket;
    const bool ok = m_backend->writer()->publishTexture(gpuResult.textureId, gpuResult.size, &ticket, &error);
    if (ok) {
        emit textureReady(ticket);
        return GpuPublishDisposition::Published;
    }

    if (!error.isEmpty()) {
        emit initializationFailed(error);
        return GpuPublishDisposition::Failed;
    }
    return GpuPublishDisposition::RetryLater;
}

ExecutionRequest PhotoEditorAppSession::buildGpuRequest() const
{
    ExecutionRequest request;
    request.requestId = ++const_cast<PhotoEditorAppSession *>(this)->m_requestSequence;
    request.typeId = QString::fromLatin1(kGpuPreviewTypeId);
    request.mergeKey = request.typeId;

    auto payload = std::make_shared<PhotoEditorGpuPreviewPayload>();
    payload->sourceKey = m_catalog->currentImagePath;
    payload->sourceImageCacheKey = m_catalog->currentImage.cacheKey();
    payload->sourceImage = m_catalog->currentImage;
    payload->parameters = m_effectParameters;
    payload->outputSize = m_outputSize;
    request.payload = payload;
    return request;
}

ExecutionRequest PhotoEditorAppSession::buildCpuRequest() const
{
    ExecutionRequest request;
    request.requestId = ++const_cast<PhotoEditorAppSession *>(this)->m_requestSequence;
    request.typeId = QString::fromLatin1(kCpuPreviewTypeId);
    request.mergeKey = request.typeId;

    auto payload = std::make_shared<PhotoEditorCpuPreviewPayload>();
    payload->sourceKey = m_catalog->currentImagePath;
    payload->sourceImageCacheKey = m_catalog->currentImage.cacheKey();
    payload->sourceImage = m_catalog->currentImage;
    payload->parameters = m_effectParameters;
    payload->previewSize = m_outputSize.boundedTo(QSize(320, 320));
    request.payload = payload;
    return request;
}
