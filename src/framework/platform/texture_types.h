#pragma once

#include "gl_types.h"

#include <QMetaType>
#include <QSize>
#include <QtGlobal>

struct TextureTicket final
{
    int slotIndex = -1;
    quint64 generation = 0;
    quint64 frameIndex = 0;
    quint64 outputRevision = 0;
    QSize size;
};

struct TextureLease final
{
    GLuint textureId = 0U;
    GLenum textureTarget = GL_TEXTURE_2D;
    QSize size;
};

Q_DECLARE_METATYPE(TextureTicket)
