#pragma once

#include "texture_types.h"

#include <QString>
#include <QSize>

class IRuntime;

namespace execution
{
class RuntimeExecutor;
}

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

    virtual void attach(execution::RuntimeExecutor *executor, IWriterEvents *events) = 0;
    virtual bool submitTexture(GLuint sourceTextureId,
                               const QSize &size,
                               quint64 outputRevision,
                               QString *error) = 0;
    virtual void notifyPresentationCapacityAvailable() = 0;
    virtual void reset() = 0;
};
