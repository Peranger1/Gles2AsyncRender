#include "photo_editor_demo_handlers.h"

#include "adapters/photo_editor/photo_editor_gles2_backend.h"
#include "framework/platform/runtime.h"
#include "photo_editor_demo_requests.h"

#include <QColor>
#include <QMetaObject>
#include <QPainter>

namespace
{
constexpr const char *kGpuPreviewTypeId = "photo_editor.preview.gpu.async";
constexpr const char *kCpuPreviewTypeId = "photo_editor.preview.cpu.sync";
thread_local IRuntime *g_photoEditorRuntime = nullptr;

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

class PhotoEditorAsyncExecution final : public IRequestExecution
{
public:
    void setCompletionCallback(std::function<void()> callback) override
    {
        m_callback = std::move(callback);
        if (m_finished && m_callback) {
            m_callback();
        }
    }

    bool collectResult(ExecutionResult *result, QString *error) override
    {
        if (!m_finished) {
            if (error) {
                *error = QStringLiteral("The async execution has not completed yet.");
            }
            return false;
        }
        if (!m_error.isEmpty()) {
            if (error) {
                *error = m_error;
            }
            return false;
        }
        if (result) {
            *result = m_result;
        }
        m_callback = nullptr;
        return true;
    }

    void markResult(const ExecutionResult &result)
    {
        m_result = result;
        m_error.clear();
        m_finished = true;
        if (m_callback) {
            m_callback();
        }
    }

    void markFailure(const QString &error)
    {
        m_error = error;
        m_finished = true;
        if (m_callback) {
            m_callback();
        }
    }

private:
    std::function<void()> m_callback;
    bool m_finished = false;
    QString m_error;
    ExecutionResult m_result;
};

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

QImage renderCpuPreview(const PhotoEditorCpuPreviewPayload &payload)
{
    const QSize targetSize = sanitizedPreviewSize(payload.previewSize, payload.sourceImage.size());
    QImage preview(targetSize, QImage::Format_ARGB32_Premultiplied);
    preview.fill(QColor(28, 28, 32));

    QPainter painter(&preview);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QSize sourceSize = payload.sourceImage.size();
    const qreal fitScale = qMin(qreal(targetSize.width()) / qMax(1, sourceSize.width()),
                                qreal(targetSize.height()) / qMax(1, sourceSize.height()));
    const qreal zoomScale = qMax(0.1, qreal(payload.parameters.zoom));
    const qreal scaleX = fitScale * zoomScale * (payload.parameters.flipHorizontal ? -1.0 : 1.0);
    const qreal scaleY = fitScale * zoomScale * (payload.parameters.flipVertical ? -1.0 : 1.0);

    painter.translate((targetSize.width() / 2.0) + (payload.parameters.panX * targetSize.width() * 0.5),
                      (targetSize.height() / 2.0) + (payload.parameters.panY * targetSize.height() * 0.5));
    painter.rotate(payload.parameters.rotationDegrees);
    painter.scale(scaleX, scaleY);
    painter.translate(-sourceSize.width() / 2.0, -sourceSize.height() / 2.0);
    painter.drawImage(QPointF(0.0, 0.0), payload.sourceImage);
    painter.end();

    applyColorAdjustments(&preview, payload.parameters.brightness, payload.parameters.contrast);
    return preview;
}
}

PhotoEditorGpuPreviewHandler::PhotoEditorGpuPreviewHandler(QObject *parent)
    : QObject(parent)
{
}

PhotoEditorGpuPreviewHandler::~PhotoEditorGpuPreviewHandler()
{
    shutdown();
}

RequestTypeDescriptor PhotoEditorGpuPreviewHandler::descriptor() const
{
    RequestTypeDescriptor descriptor;
    descriptor.typeId = QString::fromLatin1(kGpuPreviewTypeId);
    descriptor.device = DeviceKind::Gpu;
    descriptor.completion = CompletionKind::Async;
    descriptor.runtimeKind = RuntimeKind::AngleGles2;
    descriptor.queuePolicy = RequestQueuePolicyKind::MergeWhileBusy;
    return descriptor;
}

StartDisposition PhotoEditorGpuPreviewHandler::start(const ExecutionRequest &request,
                                                     IExecutionContext &context,
                                                     std::unique_ptr<IRequestExecution> *asyncExecution,
                                                     ExecutionResult *inlineResult,
                                                     QString *error)
{
    Q_UNUSED(inlineResult);

    if (asyncExecution == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorGpuPreviewHandler requires an async execution output.");
        }
        return StartDisposition::Failed;
    }
    if (m_activeExecution != nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorGpuPreviewHandler does not support concurrent requests.");
        }
        return StartDisposition::Failed;
    }

    const auto payload = std::dynamic_pointer_cast<PhotoEditorGpuPreviewPayload>(request.payload);
    if (!payload || payload->sourceImage.isNull()) {
        if (error) {
            *error = QStringLiteral("PhotoEditorGpuPreviewHandler requires a valid GPU preview payload.");
        }
        return StartDisposition::Failed;
    }

    m_runtime = context.runtime();
    if (m_runtime == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorGpuPreviewHandler requires a valid runtime.");
        }
        return StartDisposition::Failed;
    }

    if (!ensureLibraryInitialized(error)
        || !ensureSessionForPayload(payload->sourceKey,
                                    payload->sourceImageCacheKey,
                                    payload->sourceImage,
                                    payload->outputSize,
                                    error)) {
        return StartDisposition::Failed;
    }

    QString runtimeError;
    if (!m_runtime->enter(&runtimeError)) {
        if (error) {
            *error = runtimeError;
        }
        return StartDisposition::Failed;
    }

    bool ok = photo_editor_set_output_size(m_handle, sanitizedSize(payload->outputSize), error);
    if (ok) {
        ok = photo_editor_set_opcode(m_handle, payload->parameters, error);
    }
    if (ok) {
        ok = photo_editor_process(m_handle,
                                  this,
                                  &PhotoEditorGpuPreviewHandler::processProgressThunk,
                                  this,
                                  error);
    }
    m_runtime->leave();

    if (!ok) {
        return StartDisposition::Failed;
    }

    auto execution = std::make_unique<PhotoEditorAsyncExecution>();
    m_activeExecution = execution.get();
    m_activeRequestId = request.requestId;
    m_activeTypeId = request.typeId;
    *asyncExecution = std::move(execution);
    return StartDisposition::StartedAsync;
}

void PhotoEditorGpuPreviewHandler::shutdown()
{
    clearActiveExecution();
    destroySession();
    m_runtime = nullptr;
    m_libraryInitialized = false;
    m_sourceKey.clear();
    m_sourceImageCacheKey = 0;
    m_sourceImage = QImage();
}

void PhotoEditorGpuPreviewHandler::onProgressEvent(int progress, bool isEnd)
{
    Q_UNUSED(progress);

    auto *execution = dynamic_cast<PhotoEditorAsyncExecution *>(m_activeExecution);
    if (!isEnd || execution == nullptr || m_runtime == nullptr || m_handle == nullptr) {
        return;
    }

    QString error;
    if (!m_runtime->enter(&error)) {
        clearActiveExecution();
        execution->markFailure(error);
        return;
    }

    GLuint textureId = 0U;
    QSize size;
    const bool ok = photo_editor_render(m_handle, &textureId, &size, &error);
    m_runtime->leave();

    if (!ok) {
        clearActiveExecution();
        execution->markFailure(error);
        return;
    }

    ExecutionResult result;
    result.requestId = m_activeRequestId;
    result.typeId = m_activeTypeId;
    RawGpuTextureResult gpuResult;
    gpuResult.textureId = textureId;
    gpuResult.size = size;
    result.payload = gpuResult;
    clearActiveExecution();
    execution->markResult(result);
}

void PhotoEditorGpuPreviewHandler::processProgressThunk(int progress, bool isEnd, void *userData)
{
    auto *handler = static_cast<PhotoEditorGpuPreviewHandler *>(userData);
    if (handler == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(handler,
                              "onProgressEvent",
                              Qt::QueuedConnection,
                              Q_ARG(int, progress),
                              Q_ARG(bool, isEnd));
}

bool PhotoEditorGpuPreviewHandler::ensureLibraryInitialized(QString *error)
{
    if (m_libraryInitialized) {
        return true;
    }
    if (m_runtime == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorGpuPreviewHandler runtime is unavailable.");
        }
        return false;
    }

    QString runtimeError;
    if (!m_runtime->enter(&runtimeError)) {
        if (error) {
            *error = runtimeError;
        }
        return false;
    }

    g_photoEditorRuntime = m_runtime;
    const bool ok = photo_editor_init([](const char *name) -> void * {
        return g_photoEditorRuntime != nullptr ? g_photoEditorRuntime->resolveProc(name) : nullptr;
    }, error);
    g_photoEditorRuntime = nullptr;
    m_runtime->leave();

    if (ok) {
        m_libraryInitialized = true;
    }
    return ok;
}

bool PhotoEditorGpuPreviewHandler::ensureSessionForPayload(const QString &sourceKey,
                                                           quint64 sourceImageCacheKey,
                                                           const QImage &sourceImage,
                                                           const QSize &outputSize,
                                                           QString *error)
{
    if (m_handle == nullptr || m_sourceKey != sourceKey || m_sourceImageCacheKey != sourceImageCacheKey) {
        m_sourceKey = sourceKey;
        m_sourceImageCacheKey = sourceImageCacheKey;
        m_sourceImage = sourceImage;
        return rebuildSession(sourceImage, outputSize, error);
    }

    QString runtimeError;
    if (!m_runtime->enter(&runtimeError)) {
        if (error) {
            *error = runtimeError;
        }
        return false;
    }
    const bool ok = photo_editor_set_output_size(m_handle, sanitizedSize(outputSize), error);
    m_runtime->leave();
    return ok;
}

bool PhotoEditorGpuPreviewHandler::rebuildSession(const QImage &sourceImage, const QSize &outputSize, QString *error)
{
    destroySession();
    if (m_runtime == nullptr || sourceImage.isNull()) {
        if (error) {
            *error = QStringLiteral("PhotoEditorGpuPreviewHandler cannot rebuild a session without a valid runtime and source image.");
        }
        return false;
    }

    QString runtimeError;
    if (!m_runtime->enter(&runtimeError)) {
        if (error) {
            *error = runtimeError;
        }
        return false;
    }

    m_handle = photo_editor_create(sourceImage, error);
    bool ok = m_handle != nullptr;
    if (ok) {
        ok = photo_editor_set_output_size(m_handle, sanitizedSize(outputSize), error);
    }
    m_runtime->leave();

    if (!ok) {
        destroySession();
        return false;
    }
    return true;
}

void PhotoEditorGpuPreviewHandler::destroySession()
{
    if (m_handle == nullptr) {
        return;
    }

    bool entered = false;
    QString error;
    if (m_runtime != nullptr) {
        entered = m_runtime->enter(&error);
    }
    photo_editor_destroy(m_handle);
    if (entered && m_runtime != nullptr) {
        m_runtime->leave();
    }
    m_handle = nullptr;
}

void PhotoEditorGpuPreviewHandler::clearActiveExecution()
{
    m_activeExecution = nullptr;
    m_activeRequestId = 0;
    m_activeTypeId.clear();
}

RequestTypeDescriptor PhotoEditorCpuPreviewHandler::descriptor() const
{
    RequestTypeDescriptor descriptor;
    descriptor.typeId = QString::fromLatin1(kCpuPreviewTypeId);
    descriptor.device = DeviceKind::Cpu;
    descriptor.completion = CompletionKind::Sync;
    descriptor.runtimeKind = RuntimeKind::None;
    descriptor.queuePolicy = RequestQueuePolicyKind::SerialQueue;
    return descriptor;
}

StartDisposition PhotoEditorCpuPreviewHandler::start(const ExecutionRequest &request,
                                                     IExecutionContext &context,
                                                     std::unique_ptr<IRequestExecution> *asyncExecution,
                                                     ExecutionResult *inlineResult,
                                                     QString *error)
{
    Q_UNUSED(context);
    Q_UNUSED(asyncExecution);

    if (inlineResult == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorCpuPreviewHandler requires an inline result output.");
        }
        return StartDisposition::Failed;
    }

    const auto payload = std::dynamic_pointer_cast<PhotoEditorCpuPreviewPayload>(request.payload);
    if (!payload || payload->sourceImage.isNull()) {
        if (error) {
            *error = QStringLiteral("PhotoEditorCpuPreviewHandler requires a valid CPU preview payload.");
        }
        return StartDisposition::Failed;
    }

    CpuImageResult previewResult;
    previewResult.image = renderCpuPreview(*payload);
    previewResult.metadata.insert(QStringLiteral("sourceKey"), payload->sourceKey);
    previewResult.metadata.insert(QStringLiteral("sourceImageCacheKey"), qulonglong(payload->sourceImageCacheKey));
    previewResult.metadata.insert(QStringLiteral("description"),
                                  QStringLiteral("sync cpu preview %1x%2 brightness=%3 contrast=%4 zoom=%5 rotation=%6")
                                      .arg(previewResult.image.width())
                                      .arg(previewResult.image.height())
                                      .arg(payload->parameters.brightness, 0, 'f', 2)
                                      .arg(payload->parameters.contrast, 0, 'f', 2)
                                      .arg(payload->parameters.zoom, 0, 'f', 2)
                                      .arg(payload->parameters.rotationDegrees, 0, 'f', 1));

    inlineResult->requestId = request.requestId;
    inlineResult->typeId = request.typeId;
    inlineResult->payload = previewResult;
    return StartDisposition::CompletedInline;
}

void PhotoEditorCpuPreviewHandler::shutdown()
{
}
