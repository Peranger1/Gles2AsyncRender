#pragma once

#include <QMetaType>
#include <QSize>
#include <QtGlobal>

#include <QtANGLE/GLES2/gl2.h>

struct TextureTicket final
{
    int slotIndex = -1;
    quint64 generation = 0;
    quint64 frameIndex = 0;
    QSize size;
};

struct TextureLease final
{
    GLuint textureId = 0U;
    QSize size;
};

Q_DECLARE_METATYPE(TextureTicket)
