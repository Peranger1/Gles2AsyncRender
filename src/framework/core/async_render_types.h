#pragma once

#include <QSize>
#include <QtGlobal>

#include <QtANGLE/GLES2/gl2.h>

#include <memory>

struct AsyncRenderRequest final
{
    quint64 sequence = 0;
    QSize outputSize;
    std::shared_ptr<void> payload;
};

struct RenderedTexture final
{
    GLuint textureId = 0U;
    QSize size;
};

struct PublishedFrame final
{
    int slotIndex = -1;
    quintptr sharedHandle = 0;
    QSize size;
    quint64 generation = 0;
    quint64 frameIndex = 0;
};
