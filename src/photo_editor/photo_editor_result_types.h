#pragma once

#include "framework/platform/gl_types.h"

#include <QImage>
#include <QMap>
#include <QSize>
#include <QString>
#include <QVariant>

struct RawGpuTextureResult final
{
    GLuint textureId = 0U;
    QSize size;
    QMap<QString, QVariant> metadata;
};

struct CpuImageResult final
{
    QImage image;
    QMap<QString, QVariant> metadata;
};
