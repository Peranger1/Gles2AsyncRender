#include "photo_editor_app_session.h"

#include "app/photo_editor_handle_actor.h"
#include "app/photo_editor_runtime_service.h"
#include "framework/execution/runtime_executor.h"
#include "framework/platform/platform_backend.h"
#include "framework/platform/presentation_events.h"
#include "framework/platform/writer.h"
#include "runtime_diagnostics.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QPainter>

#include <exception>
#include <optional>

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

std::optional<CpuImageResult> renderCpuPreview(const std::shared_ptr<const QImage> &sourceImage,
                                               const QString &sourceKey,
                                               quint64 sourceImageCacheKey,
                                               const ImageEffectParameters &parameters,
                                               const QSize &previewSize,
                                               QString *error)
{
    if (!sourceImage || sourceImage->isNull()) {
        if (error) {
            *error = QStringLiteral("CPU preview requires a valid source image.");
        }
        return std::nullopt;
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
    return previewResult;
}

QString gpuActorKey(const QString &sourceKey, quint64 sourceImageCacheKey)
{
    return QStringLiteral("%1\n%2").arg(sourceKey).arg(sourceImageCacheKey);
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

    auto runtimeExecutor = std::make_unique<execution::RuntimeExecutor>(backend->createRuntime());
    try {
        runtimeExecutor->initialize().get();
    } catch (const std::exception &ex) {
        emit initializationFailed(QString::fromStdString(ex.what()));
        return false;
    } catch (...) {
        emit initializationFailed(QStringLiteral("Failed to initialize the platform runtime."));
        return false;
    }

    auto runtimeService = std::make_unique<PhotoEditorRuntimeService>(runtimeExecutor.get(), this);
    connect(runtimeService.get(), &PhotoEditorRuntimeService::warning, this, &PhotoEditorAppSession::requestWarning);

    m_backend = backend;
    m_runtimeExecutor = std::move(runtimeExecutor);
    m_runtimeService = std::move(runtimeService);
    if (m_backend && m_backend->writer() && m_backend->presentationEvents()) {
        m_backend->writer()->attach(m_runtimeExecutor.get(), m_backend->presentationEvents());
    }
    m_outputSize = sanitizedSize(outputSize);
    m_outputRevision = 0;
    m_initialized = true;
    m_shuttingDown = false;
    m_runtimeService->initialize();
    logSessionMessage(QStringLiteral("Initialized. App session owns the runtime host and photo editor runtime service."));
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
    if (m_initialized && m_catalog->hasImage()) {
        submitGpuPreview();
    }
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

    m_currentGpuActor.reset();
    m_gpuActors.clear();
    if (m_runtimeService) {
        m_runtimeService->shutdown();
    }
    if (m_backend && m_backend->writer()) {
        m_backend->writer()->reset();
    }
    if (m_runtimeExecutor) {
        try {
            m_runtimeExecutor->shutdown().get();
        } catch (...) {
        }
    }

    m_runtimeService.reset();
    m_runtimeExecutor.reset();
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
    std::shared_ptr<PhotoEditorHandleActor> actor = currentGpuActor();
    if (!actor) {
        return;
    }

    actor->setOutputSize(m_outputSize);
    actor->setOpcode(m_effectParameters);
    actor->process();
}

void PhotoEditorAppSession::runCpuPreviewSync()
{
    const std::shared_ptr<const QImage> sourceImage = m_catalog->currentImage;
    const QString sourceKey = m_catalog->currentImagePath;
    const quint64 sourceImageCacheKey = sourceImage ? sourceImage->cacheKey() : 0;
    const QSize previewSize = m_outputSize.boundedTo(QSize(320, 320));

    QString error;
    std::optional<CpuImageResult> previewResult = renderCpuPreview(sourceImage,
                                                                   sourceKey,
                                                                   sourceImageCacheKey,
                                                                   m_effectParameters,
                                                                   previewSize,
                                                                   &error);
    if (!previewResult.has_value()) {
        emit requestWarning(error);
        return;
    }

    const QString description = previewResult->metadata.value(QStringLiteral("description")).toString();
    emit cpuPreviewReady(previewResult->image, description);
}

QString PhotoEditorAppSession::currentGpuActorKey() const
{
    if (!m_catalog || !m_catalog->hasImage()) {
        return {};
    }
    const quint64 sourceImageCacheKey = m_catalog->currentImage ? m_catalog->currentImage->cacheKey() : 0;
    return gpuActorKey(m_catalog->currentImagePath, sourceImageCacheKey);
}

std::shared_ptr<PhotoEditorHandleActor> PhotoEditorAppSession::currentGpuActor()
{
    if (!m_runtimeService || !m_catalog->hasImage()) {
        return {};
    }

    const QString actorKey = currentGpuActorKey();
    auto actorIt = m_gpuActors.find(actorKey);
    if (actorIt != m_gpuActors.end()) {
        m_currentGpuActor = actorIt.value();
        return m_currentGpuActor;
    }

    const QString sourceKey = m_catalog->currentImagePath;
    const quint64 sourceImageCacheKey = m_catalog->currentImage ? m_catalog->currentImage->cacheKey() : 0;
    std::shared_ptr<PhotoEditorHandleActor> actor = m_runtimeService->createActor(sourceKey,
                                                                                  sourceImageCacheKey,
                                                                                  m_catalog->currentImage);
    if (!actor) {
        return {};
    }

    connect(actor.get(), &PhotoEditorHandleActor::renderResult, this, [this](const RawGpuTextureResult &result) {
        handleGpuRenderResult(result);
    });
    m_gpuActors.insert(actorKey, actor);
    m_currentGpuActor = actor;
    return m_currentGpuActor;
}

void PhotoEditorAppSession::handleGpuRenderResult(const RawGpuTextureResult &gpuResult)
{
    if (m_shuttingDown || m_backend == nullptr || m_runtimeExecutor == nullptr) {
        return;
    }

    const QString resultSourceKey = gpuResult.metadata.value(QStringLiteral("sourceKey")).toString();
    const quint64 resultSourceImageCacheKey =
        gpuResult.metadata.value(QStringLiteral("sourceImageCacheKey")).toULongLong();
    if (gpuActorKey(resultSourceKey, resultSourceImageCacheKey) != currentGpuActorKey()) {
        return;
    }

    QString error;
    if (!m_backend->writer()->submitTexture(gpuResult.textureId, gpuResult.size, m_outputRevision, &error)
        && !error.isEmpty()) {
        emit requestWarning(error);
    }
}
