#pragma once

#include "framework/core/work_types.h"
#include "image_effect_types.h"

#include <QImage>
#include <QString>

struct PhotoEditorCpuPreviewPayload final : public IWorkPayload
{
    QString sourceKey;
    quint64 sourceImageCacheKey = 0;
    QImage sourceImage;
    ImageEffectParameters parameters;
    QSize previewSize;
};
