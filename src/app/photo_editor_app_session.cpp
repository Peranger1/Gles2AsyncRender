#include "photo_editor_app_session.h"

#include "framework/execution/qt_runtime_host.h"
#include "framework/execution/runtime_scope.h"
#include "framework/platform/platform_backend.h"
#include "framework/platform/presentation_events.h"
#include "framework/platform/runtime.h"
#include "framework/platform/writer.h"
#include "photo_editor/photo_editor_gles2_backend.h"
#include "runtime_diagnostics.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>

namespace
{
thread_local IRuntime *g_photoEditorRuntime = nullptr;

void logSessionMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[PhotoEditorAppSession]", message);
}

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

QSize sanitizedPreviewSize(const QSize &requestedSize, const QSize &sourceSize)
{
    if (requestedSize.isValid()) {
        const QSize bounded = requestedSize.boundedTo(QSize(512, 512));
        return QSize(qMax(1, bounded.width()), qMax(1, bounded.height()));
    }

    if (sourceSize.isValid()) {
        const QSize scaled = sourceSize.scaled(QSize(320, 320), Qt::KeepAspectRatio);
        return QSize(qMax(1, scaled.width()), qMax(1, scaled.height()));
    }

    return QSize(320, 320);
}

QString currentDisplayName(const QStringList &imagePaths, int currentImageIndex)
{
    if (currentImageIndex < 0 || currentImageIndex >= imagePaths.size()) {
        return {};
    }
    return QFileInfo(imagePaths[currentImageIndex]).fileName();
}

int adjustedChannel(int channel, float brightness, float contrast)
{
    const float normalized = float(channel) / 255.0f;
    const float adjusted = ((normalized - 0.5f) * contrast) + 0.5f + brightness;
    const int result = qRound(adjusted * 255.0f);
    return qBound(0, result, 255);
}

void applyColorAdjustments(QImage *image, float brightness, float contrast)
{
    if (image == nullptr || image->isNull()) {
        return;
    }

    QImage argb = image->convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < argb.height(); ++y) {
        QRgb *scanLine = reinterpret_cast<QRgb *>(argb.scanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            const QRgb pixel = scanLine[x];
            scanLine[x] = qRgba(adjustedChannel(qRed(pixel), brightness, contrast),
                                adjustedChannel(qGreen(pixel), brightness, contrast),
                                adjustedChannel(qBlue(pixel), brightness, contrast),
                                qAlpha(pixel));
        }
    }

    *image = std::move(argb);
}

ExecutionOutcome<CpuImageResult> renderCpuPreview(const std::shared_ptr<const QImage> &sourceImage,
                                                  const QString &sourceKey,
                                                  quint64 sourceImageCacheKey,
                                                  const ImageEffectParameters &parameters,
                                                  const QSize &previewSize)
{
    if (!sourceImage || sourceImage->isNull()) {
        ExecutionError error;
        error.message = QStringLiteral("CPU preview requires a valid source image.");
        return ExecutionOutcome<CpuImageResult>::failure(std::move(error));
    }

    const QSize targetSize = sanitizedPreviewSize(previewSize, sourceImage->size());
    QImage preview(targetSize, QImage::Format_ARGB32_Premultiplied);
    preview.fill(QColor(28, 28, 32));

    QPainter painter(&preview);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QSize sourceSize = sourceImage->size();
    const qreal fitScale = qMin(qreal(targetSize.width()) / qMax(1, sourceSize.width()),
                                qreal(targetSize.height()) / qMax(1, sourceSize.height()));
    const qreal zoomScale = qMax(0.1, qreal(parameters.zoom));
    const qreal scaleX = fitScale * zoomScale * (parameters.flipHorizontal ? -1.0 : 1.0);
    const qreal scaleY = fitScale * zoomScale * (parameters.flipVertical ? -1.0 : 1.0);

    painter.translate((targetSize.width() / 2.0) + (parameters.panX * targetSize.width() * 0.5),
                      (targetSize.height() / 2.0) + (parameters.panY * targetSize.height() * 0.5));
    painter.rotate(parameters.rotationDegrees);
    painter.scale(scaleX, scaleY);
    painter.translate(-sourceSize.width() / 2.0, -sourceSize.height() / 2.0);
    painter.drawImage(QPointF(0.0, 0.0), *sourceImage);
    painter.end();

    applyColorAdjustments(&preview, parameters.brightness, parameters.contrast);

    CpuImageResult previewResult;
    previewResult.image = std::move(preview);
    previewResult.metadata.insert(QStringLiteral("sourceKey"), sourceKey);
    previewResult.metadata.insert(QStringLiteral("sourceImageCacheKey"), qulonglong(sourceImageCacheKey));
    previewResult.metadata.insert(QStringLiteral("description"),
                                  QStringLiteral("sync cpu preview %1x%2 brightness=%3 contrast=%4 zoom=%5 rotation=%6")
                                      .arg(previewResult.image.width())
                                      .arg(previewResult.image.height())
                                      .arg(parameters.brightness, 0, 'f', 2)
                                      .arg(parameters.contrast, 0, 'f', 2)
                                      .arg(parameters.zoom, 0, 'f', 2)
                                      .arg(parameters.rotationDegrees, 0, 'f', 1));
    return ExecutionOutcome<CpuImageResult>::success(std::move(previewResult));
}

ExecutionError makeError(const QString &message)
{
    ExecutionError error;
    error.message = message;
    return error;
}
}

struct PhotoEditorAppSession::ImageCatalogState final
{
    QStringList imagePaths;
    int currentImageIndex = -1;
    std::shared_ptr<const QImage> currentImage;
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
        return currentImage && !currentImage->isNull();
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
        auto loadedImage = std::make_shared<const QImage>(currentImagePath);
        if (loadedImage->isNull()) {
            if (error) {
                *error = QStringLiteral("Failed to load the first image from the selected directory.");
            }
            return false;
        }
        currentImage = std::move(loadedImage);
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
        auto loadedImage = std::make_shared<const QImage>(currentImagePath);
        if (loadedImage->isNull()) {
            if (error) {
                *error = QStringLiteral("Failed to load image: %1").arg(currentImagePath);
            }
            return false;
        }
        currentImage = std::move(loadedImage);
        return true;
    }
};

struct PhotoEditorAppSession::GpuProcessBridge final
{
    QPointer<PhotoEditorAppSession> session;
    IRuntime *runtime = nullptr;
    void *handle = nullptr;
    TaskId taskId = 0;
    GpuPreviewLane::Done done;
    bool canceled = false;
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
    auto runtimeHost = std::make_unique<QtRuntimeHost>(std::move(runtime));
    QString error;
    if (!runtimeHost->start(&error)) {
        emit initializationFailed(error.isEmpty()
                                      ? QStringLiteral("Failed to initialize the platform runtime.")
                                      : error);
        return false;
    }

    auto previewLane = std::make_unique<GpuPreviewLane>(
        runtimeHost.get(),
        [this](const TaskContext &context, const GpuPreviewRequest &request, GpuPreviewLane::Done done) {
            runGpuPreviewStep(context, request, std::move(done));
        });

    m_backend = backend;
    m_runtimeHost = std::move(runtimeHost);
    m_gpuPreviewLane = std::move(previewLane);
    if (m_backend && m_backend->writer() && m_backend->presentationEvents()) {
        m_backend->writer()->attach(m_runtimeHost.get(), m_runtimeHost->runtime(), m_backend->presentationEvents());
    }
    m_outputSize = sanitizedSize(outputSize);
    m_outputRevision = 0;
    m_initialized = true;
    m_shuttingDown = false;
    logSessionMessage(QStringLiteral("Initialized. App session owns the runtime host and template preview lane."));
    return true;
}

void PhotoEditorAppSession::setOutputSize(QSize size, quint64 outputRevision)
{
    if (m_shuttingDown) {
        return;
    }

    const QSize safeSize = sanitizedSize(size);
    const bool sizeChanged = m_outputSize != safeSize;
    const bool revisionChanged = m_outputRevision != outputRevision;
    if (!sizeChanged && !revisionChanged) {
        return;
    }

    m_outputSize = safeSize;
    m_outputRevision = outputRevision;
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
                                    m_catalog->currentImage ? m_catalog->currentImage->size() : QSize());
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

void PhotoEditorAppSession::shutdown()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;
    m_initialized = false;

    if (m_gpuPreviewLane) {
        m_gpuPreviewLane->shutdown();
    }
    if (m_activeGpuBridge && m_activeGpuBridge->done) {
        ExecutionError error;
        error.state = TaskState::Shutdown;
        error.message = QStringLiteral("The GPU preview was shut down before it completed.");
        auto done = std::move(m_activeGpuBridge->done);
        m_activeGpuBridge->canceled = true;
        done(ExecutionOutcome<RawGpuTextureResult>::failure(std::move(error)));
    }
    destroyGpuEditor();
    m_activeGpuBridge.reset();
    if (m_backend && m_backend->writer()) {
        m_backend->writer()->reset();
    }
    if (m_runtimeHost) {
        m_runtimeHost->shutdown();
    }

    m_gpuPreviewLane.reset();
    m_runtimeHost.reset();
    m_backend = nullptr;
    m_catalog = std::make_unique<ImageCatalogState>();
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
                               m_catalog->currentImage ? m_catalog->currentImage->size() : QSize());
}

void PhotoEditorAppSession::submitGpuPreview()
{
    if (!m_gpuPreviewLane || !m_catalog->hasImage()) {
        return;
    }

    GpuPreviewRequest request;
    request.sourceKey = m_catalog->currentImagePath;
    request.sourceImageCacheKey = m_catalog->currentImage ? m_catalog->currentImage->cacheKey() : 0;
    request.sourceImage = m_catalog->currentImage;
    request.parameters = m_effectParameters;
    request.outputSize = m_outputSize;

    const SubmitResult submit = m_gpuPreviewLane->submit(
        std::move(request),
        [this](TaskId taskId, ExecutionOutcome<RawGpuTextureResult> outcome) {
            handleGpuPreviewCompleted(taskId, std::move(outcome));
        });
    if (!submit.accepted && !submit.error.message.isEmpty()) {
        emit requestWarning(submit.error.message);
    }
}

void PhotoEditorAppSession::runCpuPreviewSync()
{
    const std::shared_ptr<const QImage> sourceImage = m_catalog->currentImage;
    const QString sourceKey = m_catalog->currentImagePath;
    const quint64 sourceImageCacheKey = sourceImage ? sourceImage->cacheKey() : 0;
    const QSize previewSize = m_outputSize.boundedTo(QSize(320, 320));

    const ExecutionOutcome<CpuImageResult> outcome = renderCpuPreview(sourceImage,
                                                                      sourceKey,
                                                                      sourceImageCacheKey,
                                                                      m_effectParameters,
                                                                      previewSize);
    if (!outcome.ok()) {
        emit requestWarning(outcome.error().message);
        return;
    }

    const CpuImageResult &previewResult = outcome.value();
    const QString description = previewResult.metadata.value(QStringLiteral("description")).toString();
    emit cpuPreviewReady(previewResult.image, description);
}

void PhotoEditorAppSession::destroyGpuEditor()
{
    if (m_gpuEditorHandle == nullptr || m_runtimeHost == nullptr) {
        m_gpuEditorHandle = nullptr;
        m_gpuSourceKey.clear();
        m_gpuSourceImageCacheKey = 0;
        m_gpuSourceImage.reset();
        m_photoEditorInitialized = false;
        return;
    }

    QString ignoredError;
    RuntimeScope scope(m_runtimeHost->runtime(), &ignoredError);
    Q_UNUSED(ignoredError);
    if (scope.ok()) {
        photo_editor_destroy(m_gpuEditorHandle);
    }
    m_gpuEditorHandle = nullptr;
    m_gpuSourceKey.clear();
    m_gpuSourceImageCacheKey = 0;
    m_gpuSourceImage.reset();
    m_photoEditorInitialized = false;
}

ExecutionOutcome<void> PhotoEditorAppSession::ensurePhotoEditorInitialized(IRuntime *runtime)
{
    if (m_photoEditorInitialized) {
        return ExecutionOutcome<void>::success();
    }
    if (runtime == nullptr) {
        return ExecutionOutcome<void>::failure(makeError(QStringLiteral("The photo editor runtime is unavailable.")));
    }

    QString errorText;
    RuntimeScope scope(runtime, &errorText);
    if (!scope.ok()) {
        return ExecutionOutcome<void>::failure(makeError(errorText));
    }

    g_photoEditorRuntime = runtime;
    const bool ok = photo_editor_init([](const char *name) -> void * {
        return g_photoEditorRuntime ? g_photoEditorRuntime->resolveProc(name) : nullptr;
    }, &errorText);
    g_photoEditorRuntime = nullptr;

    if (!ok) {
        return ExecutionOutcome<void>::failure(makeError(errorText));
    }

    m_photoEditorInitialized = true;
    return ExecutionOutcome<void>::success();
}

ExecutionOutcome<void> PhotoEditorAppSession::ensureGpuEditor(IRuntime *runtime,
                                                              const std::shared_ptr<const QImage> &sourceImage,
                                                              const QString &sourceKey,
                                                              quint64 sourceImageCacheKey,
                                                              QSize outputSize)
{
    if (runtime == nullptr || !sourceImage || sourceImage->isNull()) {
        return ExecutionOutcome<void>::failure(makeError(QStringLiteral("GPU preview requires a valid runtime and source image.")));
    }

    ExecutionOutcome<void> initOutcome = ensurePhotoEditorInitialized(runtime);
    if (!initOutcome.ok()) {
        return initOutcome;
    }

    const bool needsCreate = m_gpuEditorHandle == nullptr
        || m_gpuSourceKey != sourceKey
        || m_gpuSourceImageCacheKey != sourceImageCacheKey;

    QString errorText;
    RuntimeScope scope(runtime, &errorText);
    if (!scope.ok()) {
        return ExecutionOutcome<void>::failure(makeError(errorText));
    }

    if (needsCreate) {
        if (m_gpuEditorHandle != nullptr) {
            photo_editor_destroy(m_gpuEditorHandle);
            m_gpuEditorHandle = nullptr;
        }

        m_gpuEditorHandle = photo_editor_create(*sourceImage, &errorText);
        if (m_gpuEditorHandle == nullptr) {
            return ExecutionOutcome<void>::failure(makeError(errorText.isEmpty()
                                                                 ? QStringLiteral("photo_editor_create failed.")
                                                                 : errorText));
        }

        m_gpuSourceKey = sourceKey;
        m_gpuSourceImageCacheKey = sourceImageCacheKey;
        m_gpuSourceImage = sourceImage;
    }

    if (!photo_editor_set_output_size(m_gpuEditorHandle, sanitizedSize(outputSize), &errorText)) {
        return ExecutionOutcome<void>::failure(makeError(errorText));
    }

    return ExecutionOutcome<void>::success();
}

void PhotoEditorAppSession::runGpuPreviewStep(const TaskContext &context,
                                              const GpuPreviewRequest &request,
                                              GpuPreviewLane::Done done)
{
    if (!done) {
        return;
    }
    if (m_activeGpuBridge) {
        done(ExecutionOutcome<RawGpuTextureResult>::failure(
            makeError(QStringLiteral("GPU preview does not support concurrent photo_editor_process calls."))));
        return;
    }

    ExecutionOutcome<void> editorOutcome = ensureGpuEditor(context.runtime,
                                                           request.sourceImage,
                                                           request.sourceKey,
                                                           request.sourceImageCacheKey,
                                                           request.outputSize);
    if (!editorOutcome.ok()) {
        done(ExecutionOutcome<RawGpuTextureResult>::failure(editorOutcome.error()));
        return;
    }

    QString errorText;
    RuntimeScope scope(context.runtime, &errorText);
    if (!scope.ok()) {
        done(ExecutionOutcome<RawGpuTextureResult>::failure(makeError(errorText)));
        return;
    }

    bool ok = photo_editor_set_output_size(m_gpuEditorHandle, sanitizedSize(request.outputSize), &errorText);
    if (ok) {
        ok = photo_editor_set_opcode(m_gpuEditorHandle, request.parameters, &errorText);
    }

    if (!ok) {
        done(ExecutionOutcome<RawGpuTextureResult>::failure(makeError(errorText)));
        return;
    }

    auto bridge = std::make_shared<GpuProcessBridge>();
    bridge->session = this;
    bridge->runtime = context.runtime;
    bridge->handle = m_gpuEditorHandle;
    bridge->taskId = context.taskId;
    bridge->done = std::move(done);

    ok = photo_editor_process(m_gpuEditorHandle,
                              this,
                              &PhotoEditorAppSession::onGpuProcessProgress,
                              bridge.get(),
                              &errorText);
    if (!ok) {
        done = std::move(bridge->done);
        done(ExecutionOutcome<RawGpuTextureResult>::failure(makeError(errorText)));
        return;
    }

    m_activeGpuBridge = std::move(bridge);
}

ExecutionOutcome<RawGpuTextureResult> PhotoEditorAppSession::collectGpuRenderResult(IRuntime *runtime, void *handle)
{
    if (runtime == nullptr || handle == nullptr) {
        return ExecutionOutcome<RawGpuTextureResult>::failure(
            makeError(QStringLiteral("GPU preview render requires an active editor handle.")));
    }

    QString errorText;
    RuntimeScope scope(runtime, &errorText);
    if (!scope.ok()) {
        return ExecutionOutcome<RawGpuTextureResult>::failure(makeError(errorText));
    }

    GLuint textureId = 0U;
    QSize size;
    if (!photo_editor_render(handle, &textureId, &size, &errorText)) {
        return ExecutionOutcome<RawGpuTextureResult>::failure(makeError(errorText));
    }

    RawGpuTextureResult gpuResult;
    gpuResult.textureId = textureId;
    gpuResult.size = size;
    return ExecutionOutcome<RawGpuTextureResult>::success(std::move(gpuResult));
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
    QString error;
    if (!m_backend->writer()->submitTexture(gpuResult.textureId, gpuResult.size, m_outputRevision, &error)
        && !error.isEmpty()) {
        emit requestWarning(error);
    }
}

void PhotoEditorAppSession::onGpuProcessProgress(int progress, bool isEnd, void *userData)
{
    Q_UNUSED(progress);

    auto *bridge = static_cast<GpuProcessBridge *>(userData);
    if (bridge == nullptr || bridge->session.isNull() || !isEnd) {
        return;
    }

    PhotoEditorAppSession *session = bridge->session.data();
    if (!session->m_activeGpuBridge || session->m_activeGpuBridge.get() != bridge) {
        return;
    }

    auto done = std::move(session->m_activeGpuBridge->done);
    const bool canceled = session->m_activeGpuBridge->canceled;
    ExecutionOutcome<RawGpuTextureResult> result = canceled
        ? ExecutionOutcome<RawGpuTextureResult>::failure({ TaskState::Shutdown,
                                                           QStringLiteral("The GPU preview was canceled before render."),
                                                           false })
        : session->collectGpuRenderResult(bridge->runtime, bridge->handle);
    session->m_activeGpuBridge.reset();
    if (done) {
        done(std::move(result));
    }
}
