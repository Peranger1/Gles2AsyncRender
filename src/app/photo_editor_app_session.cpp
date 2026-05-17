#include "photo_editor_app_session.h"

#include "framework/execution/execution_types.h"
#include "framework/platform/platform_backend.h"
#include "framework/platform/runtime.h"
#include "framework/platform/writer.h"
#include "photo_editor/photo_editor_cpu_renderer.h"
#include "photo_editor/photo_editor_runtime_host.h"
#include "runtime_diagnostics.h"

#include <QDir>
#include <QFileInfo>

namespace
{
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

    std::unique_ptr<IRuntime> runtime = backend->createRuntime();
    auto runtimeHost = std::make_unique<PhotoEditorRuntimeHost>(std::move(runtime));
    QString error;
    if (!runtimeHost->start(&error)) {
        emit initializationFailed(error.isEmpty()
                                      ? QStringLiteral("Failed to initialize the platform runtime.")
                                      : error);
        return false;
    }

    auto invoker = std::make_unique<RuntimeInvoker>(runtimeHost.get());
    auto gpuSession = std::make_unique<PhotoEditorGpuSession>();
    auto previewLane = std::make_unique<AsyncLane<PhotoEditorGpuPreviewArgs, RawGpuTextureResult>>(
        runtimeHost.get(),
        LaneConfig { QStringLiteral("photo_editor.preview.gpu"),
                     QueuePolicyKind::MergeWhileBusy,
                     DeliveryPolicyKind::DeliverEveryStartedResult,
                     1 },
        [this, gpuSessionPtr = gpuSession.get()](const TaskContext &context,
                                                 const PhotoEditorGpuPreviewArgs &args,
                                                 AsyncLane<PhotoEditorGpuPreviewArgs, RawGpuTextureResult>::Done done) {
            gpuSessionPtr->renderPreviewAsync(context, args, std::move(done));
        },
        std::make_shared<PhotoEditorGpuPreviewArgsMerger>());

    m_backend = backend;
    m_runtimeHost = std::move(runtimeHost);
    m_invoker = std::move(invoker);
    m_gpuSession = std::move(gpuSession);
    m_gpuPreviewLane = std::move(previewLane);
    m_outputSize = sanitizedSize(outputSize);
    m_initialized = true;
    m_shuttingDown = false;
    m_hasPendingGpuPublish = false;
    m_pendingGpuRuntime = nullptr;
    logSessionMessage(QStringLiteral("Initialized. App session now owns the runtime host and preview lane."));
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
        submitGpuPreview();
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
    submitGpuPreview();
}

void PhotoEditorAppSession::selectNextImage()
{
    if (!m_initialized || m_shuttingDown) {
        return;
    }

    QString error;
    if (!m_catalog->selectRelativeImage(1, &error)) {
        if (!error.isEmpty()) {
            emit requestWarning(error);
        }
        return;
    }

    emitImageSelection();
    submitGpuPreview();
}

void PhotoEditorAppSession::selectPreviousImage()
{
    if (!m_initialized || m_shuttingDown) {
        return;
    }

    QString error;
    if (!m_catalog->selectRelativeImage(-1, &error)) {
        if (!error.isEmpty()) {
            emit requestWarning(error);
        }
        return;
    }

    emitImageSelection();
    submitGpuPreview();
}

void PhotoEditorAppSession::requestRender()
{
    if (m_initialized && !m_shuttingDown && m_catalog->hasImage()) {
        submitGpuPreview();
    }
}

void PhotoEditorAppSession::requestCpuPreview()
{
    if (m_initialized && !m_shuttingDown && m_catalog->hasImage()) {
        runCpuPreviewSync();
    }
}

void PhotoEditorAppSession::onTextureConsumed()
{
    if (m_shuttingDown || !m_hasPendingGpuPublish) {
        return;
    }

    if (tryPublishGpuResult(m_pendingGpuPublish, m_pendingGpuRuntime) == GpuPublishDisposition::Published) {
        m_hasPendingGpuPublish = false;
        m_pendingGpuPublish = {};
        m_pendingGpuRuntime = nullptr;
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
    m_pendingGpuRuntime = nullptr;

    if (m_gpuPreviewLane) {
        m_gpuPreviewLane->shutdown();
    }
    if (m_gpuSession && m_runtimeHost) {
        m_invoker->invokeSync<void>([this](IRuntime *runtime) {
            m_gpuSession->shutdown(runtime);
            return ExecutionOutcome<void>::success();
        });
    }
    if (m_backend && m_backend->writer()) {
        m_backend->writer()->reset();
    }
    if (m_runtimeHost) {
        m_runtimeHost->shutdown();
    }

    m_gpuPreviewLane.reset();
    m_gpuSession.reset();
    m_invoker.reset();
    m_runtimeHost.reset();
    m_backend = nullptr;
    m_catalog = std::make_unique<ImageCatalogState>();
}

bool PhotoEditorAppSession::PhotoEditorGpuPreviewArgsMerger::canMerge(const PhotoEditorGpuPreviewArgs &waiting,
                                                                      const PhotoEditorGpuPreviewArgs &incoming) const
{
    return waiting.source.sourceKey == incoming.source.sourceKey;
}

PhotoEditorGpuPreviewArgs PhotoEditorAppSession::PhotoEditorGpuPreviewArgsMerger::merge(
    const PhotoEditorGpuPreviewArgs &waiting,
    const PhotoEditorGpuPreviewArgs &incoming) const
{
    Q_UNUSED(waiting);
    return incoming;
}

PhotoEditorSourceSnapshot PhotoEditorAppSession::currentSourceSnapshot() const
{
    PhotoEditorSourceSnapshot snapshot;
    snapshot.sourceKey = m_catalog->currentImagePath;
    snapshot.sourceImageCacheKey = m_catalog->currentImage.cacheKey();
    snapshot.sourceImage = m_catalog->currentImage;
    return snapshot;
}

PhotoEditorGpuPreviewArgs PhotoEditorAppSession::buildGpuPreviewArgs() const
{
    PhotoEditorGpuPreviewArgs args;
    args.source = currentSourceSnapshot();
    args.parameters = m_effectParameters;
    args.outputSize = m_outputSize;
    return args;
}

PhotoEditorCpuPreviewArgs PhotoEditorAppSession::buildCpuPreviewArgs() const
{
    PhotoEditorCpuPreviewArgs args;
    args.source = currentSourceSnapshot();
    args.parameters = m_effectParameters;
    args.previewSize = m_outputSize.boundedTo(QSize(320, 320));
    return args;
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

void PhotoEditorAppSession::submitGpuPreview()
{
    if (!m_gpuPreviewLane) {
        return;
    }

    const SubmitResult submit = m_gpuPreviewLane->submit(
        buildGpuPreviewArgs(),
        [this](TaskId taskId, ExecutionOutcome<RawGpuTextureResult> outcome) {
            handleGpuPreviewCompleted(taskId, std::move(outcome));
        });
    if (!submit.accepted && !submit.error.message.isEmpty()) {
        emit requestWarning(submit.error.message);
    }
}

void PhotoEditorAppSession::runCpuPreviewSync()
{
    if (!m_invoker) {
        return;
    }

    const PhotoEditorCpuPreviewArgs args = buildCpuPreviewArgs();
    const ExecutionOutcome<CpuImageResult> outcome = m_invoker->callDirect<CpuImageResult>([args]() {
        return PhotoEditorCpuRenderer::renderPreview(args);
    });
    if (!outcome.ok()) {
        emit requestWarning(outcome.error().message);
        return;
    }

    const CpuImageResult &previewResult = outcome.value();
    const QString description = previewResult.metadata.value(QStringLiteral("description")).toString();
    emit cpuPreviewReady(previewResult.image, description);
}

void PhotoEditorAppSession::handleGpuPreviewCompleted(TaskId taskId,
                                                      ExecutionOutcome<RawGpuTextureResult> outcome)
{
    Q_UNUSED(taskId);

    if (m_shuttingDown || m_backend == nullptr || m_runtimeHost == nullptr) {
        return;
    }
    if (!outcome.ok()) {
        emit requestWarning(outcome.error().message);
        return;
    }

    const RawGpuTextureResult &gpuResult = outcome.value();
    const GpuPublishDisposition disposition = tryPublishGpuResult(gpuResult, m_runtimeHost->runtime());
    if (disposition == GpuPublishDisposition::RetryLater) {
        m_pendingGpuPublish = gpuResult;
        m_pendingGpuRuntime = m_runtimeHost->runtime();
        m_hasPendingGpuPublish = true;
    }
}

PhotoEditorAppSession::GpuPublishDisposition PhotoEditorAppSession::tryPublishGpuResult(
    const RawGpuTextureResult &gpuResult,
    IRuntime *sourceRuntime)
{
    if (m_backend == nullptr || m_backend->writer() == nullptr) {
        return GpuPublishDisposition::Failed;
    }

    if (sourceRuntime == nullptr) {
        emit requestWarning(QStringLiteral("The GPU result cannot be published because the execution context runtime is unavailable."));
        return GpuPublishDisposition::Failed;
    }

    if (m_runtimeHost && sourceRuntime != m_runtimeHost->runtime()) {
        emit requestWarning(QStringLiteral("The GPU result runtime does not match the app session runtime."));
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
        emit requestWarning(error);
        return GpuPublishDisposition::Failed;
    }
    return GpuPublishDisposition::RetryLater;
}
