#include "photo_editor_cpu_renderer.h"

#include <QColor>
#include <QPainter>

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
}

ExecutionOutcome<CpuImageResult> PhotoEditorCpuRenderer::renderPreview(const PhotoEditorCpuPreviewArgs &args)
{
    if (args.source.sourceImage.isNull()) {
        ExecutionError error;
        error.message = QStringLiteral("PhotoEditorCpuRenderer requires a valid source image.");
        return ExecutionOutcome<CpuImageResult>::failure(std::move(error));
    }

    const QSize targetSize = sanitizedPreviewSize(args.previewSize, args.source.sourceImage.size());
    QImage preview(targetSize, QImage::Format_ARGB32_Premultiplied);
    preview.fill(QColor(28, 28, 32));

    QPainter painter(&preview);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QSize sourceSize = args.source.sourceImage.size();
    const qreal fitScale = qMin(qreal(targetSize.width()) / qMax(1, sourceSize.width()),
                                qreal(targetSize.height()) / qMax(1, sourceSize.height()));
    const qreal zoomScale = qMax(0.1, qreal(args.parameters.zoom));
    const qreal scaleX = fitScale * zoomScale * (args.parameters.flipHorizontal ? -1.0 : 1.0);
    const qreal scaleY = fitScale * zoomScale * (args.parameters.flipVertical ? -1.0 : 1.0);

    painter.translate((targetSize.width() / 2.0) + (args.parameters.panX * targetSize.width() * 0.5),
                      (targetSize.height() / 2.0) + (args.parameters.panY * targetSize.height() * 0.5));
    painter.rotate(args.parameters.rotationDegrees);
    painter.scale(scaleX, scaleY);
    painter.translate(-sourceSize.width() / 2.0, -sourceSize.height() / 2.0);
    painter.drawImage(QPointF(0.0, 0.0), args.source.sourceImage);
    painter.end();

    applyColorAdjustments(&preview, args.parameters.brightness, args.parameters.contrast);

    CpuImageResult previewResult;
    previewResult.image = std::move(preview);
    previewResult.metadata.insert(QStringLiteral("sourceKey"), args.source.sourceKey);
    previewResult.metadata.insert(QStringLiteral("sourceImageCacheKey"), qulonglong(args.source.sourceImageCacheKey));
    previewResult.metadata.insert(QStringLiteral("description"),
                                  QStringLiteral("sync cpu preview %1x%2 brightness=%3 contrast=%4 zoom=%5 rotation=%6")
                                      .arg(previewResult.image.width())
                                      .arg(previewResult.image.height())
                                      .arg(args.parameters.brightness, 0, 'f', 2)
                                      .arg(args.parameters.contrast, 0, 'f', 2)
                                      .arg(args.parameters.zoom, 0, 'f', 2)
                                      .arg(args.parameters.rotationDegrees, 0, 'f', 1));
    return ExecutionOutcome<CpuImageResult>::success(std::move(previewResult));
}
