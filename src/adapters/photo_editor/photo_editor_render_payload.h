#pragma once

#include "src/image_effect_types.h"

#include <QImage>
#include <QString>

struct PhotoEditorRenderPayload final
{
    QString sourceKey;
    quint64 sourceImageCacheKey = 0;
    QImage sourceImage;
    ImageEffectParameters parameters;
};
