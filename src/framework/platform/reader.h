#pragma once

#include "texture_types.h"

#include <QString>

class IReader
{
public:
    virtual ~IReader() = default;

    virtual bool attachToCurrentContext(QString *error) = 0;
    virtual bool acquire(const TextureTicket &ticket, TextureLease *lease, QString *error) = 0;
    virtual void release(const TextureLease &lease) = 0;
    virtual void detach() = 0;
};
