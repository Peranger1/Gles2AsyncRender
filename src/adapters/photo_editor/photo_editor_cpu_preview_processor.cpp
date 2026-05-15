#include "photo_editor_cpu_preview_processor.h"

#include "photo_editor_cpu_preview_payload.h"

#include <QColor>
#include <QPainter>
#include <QVariant>

namespace
{
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

bool PhotoEditorCpuPreviewProcessor::initialize(IWorkRuntime &runtime, QString *error)
{
    Q_UNUSED(runtime);
    Q_UNUSED(error);
    return true;
}

void PhotoEditorCpuPreviewProcessor::setWakeCallback(ProcessorWakeCallback callback)
{
    m_wakeCallback = std::move(callback);
}

bool PhotoEditorCpuPreviewProcessor::start(const WorkEnvelope &work,
                                           IWorkObserver *observer,
                                           QString *error)
{
    if (m_hasOutput) {
        if (error) {
            *error = QStringLiteral("PhotoEditorCpuPreviewProcessor does not support concurrent work.");
        }
        return false;
    }

    const auto payload = std::dynamic_pointer_cast<PhotoEditorCpuPreviewPayload>(work.payload);
    if (!payload || payload->sourceImage.isNull()) {
        if (error) {
            *error = QStringLiteral("PhotoEditorCpuPreviewProcessor requires a valid image payload.");
        }
        return false;
    }

    if (observer) {
        observer->onStateChanged(work.requestId, WorkState::Admitted);
        observer->onStateChanged(work.requestId, WorkState::Executing);
        observer->onProgress(work.requestId, 100, true);
        observer->onMessage(work.requestId, QStringLiteral("Synchronous CPU preview request completed inline."));
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

    m_output = {};
    m_output.requestId = work.requestId;
    m_output.outputKind = QStringLiteral("photo_editor.cpu_preview");
    m_output.payload = previewResult;
    m_outputReady = true;
    m_hasOutput = true;

    if (m_wakeCallback) {
        m_wakeCallback();
    }

    return true;
}

bool PhotoEditorCpuPreviewProcessor::isOutputReady() const
{
    return m_hasOutput && m_outputReady;
}

bool PhotoEditorCpuPreviewProcessor::collectOutputIfReady(ProcessorOutput *output, QString *error)
{
    if (output == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorCpuPreviewProcessor requires a valid output target.");
        }
        return false;
    }

    if (!m_hasOutput || !m_outputReady) {
        if (error) {
            error->clear();
        }
        return false;
    }

    *output = m_output;
    m_output = {};
    m_outputReady = false;
    m_hasOutput = false;
    return true;
}

void PhotoEditorCpuPreviewProcessor::shutdown()
{
    m_output = {};
    m_outputReady = false;
    m_hasOutput = false;
    m_wakeCallback = {};
}
