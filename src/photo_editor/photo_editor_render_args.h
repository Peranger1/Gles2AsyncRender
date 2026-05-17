#pragma once

#include "image_effect_types.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

struct PhotoEditorSourceSnapshot final
{
    QString sourceKey;
    quint64 sourceImageCacheKey = 0;
    QImage sourceImage;
};

struct PhotoEditorGpuPreviewArgs final
{
    PhotoEditorSourceSnapshot source;
    ImageEffectParameters parameters;
    QSize outputSize;
};

struct PhotoEditorCpuPreviewArgs final
{
    PhotoEditorSourceSnapshot source;
    ImageEffectParameters parameters;
    QSize previewSize;
};
