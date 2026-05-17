#pragma once

#include "texture_types.h"

#include <QString>
#include <QSize>

class IRuntime;
class RuntimeHost;

class IWriterEvents
{
public:
    virtual ~IWriterEvents() = default;

    virtual void onTextureReady(const TextureTicket &ticket) = 0;
    virtual void onWarning(const QString &reason) = 0;
};

class IWriter
{
public:
    virtual ~IWriter() = default;

    virtual void attach(RuntimeHost *host, IRuntime *runtime, IWriterEvents *events) = 0;
    virtual bool submitTexture(GLuint sourceTextureId,
                               const QSize &size,
                               quint64 outputRevision,
                               QString *error) = 0;
    virtual void notifyPresentationCapacityAvailable() = 0;
    virtual void reset() = 0;
};
