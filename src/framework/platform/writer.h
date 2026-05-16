#pragma once

#include "texture_types.h"

#include <QString>

class IWriter
{
public:
    virtual ~IWriter() = default;

    virtual bool publishTexture(GLuint sourceTextureId,
                                const QSize &size,
                                TextureTicket *ticket,
                                QString *error) = 0;
    virtual void reset() = 0;
};
