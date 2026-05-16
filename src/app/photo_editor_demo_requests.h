#pragma once

#include "framework/execution/execution_types.h"
#include "image_effect_types.h"

#include <QImage>
#include <QSize>
#include <QString>

struct PhotoEditorGpuPreviewPayload final : public IRequestPayload
{
    QString sourceKey;
    quint64 sourceImageCacheKey = 0;
    QImage sourceImage;
    ImageEffectParameters parameters;
    QSize outputSize;
};

struct PhotoEditorCpuPreviewPayload final : public IRequestPayload
{
    QString sourceKey;
    quint64 sourceImageCacheKey = 0;
    QImage sourceImage;
    ImageEffectParameters parameters;
    QSize previewSize;
};
