#pragma once

#include <QSize>
#include <QString>

#include <QtANGLE/GLES2/gl2.h>

class IRenderRuntime;
class ISharedFrameSlotPool;

class IFramePublisher
{
public:
    virtual ~IFramePublisher() = default;

    virtual bool initialize(IRenderRuntime *runtime,
                            ISharedFrameSlotPool *slotPool,
                            QString *error) = 0;

    virtual bool publishToSlot(GLuint sourceTextureId,
                               const QSize &sourceSize,
                               int slotIndex,
                               QString *error) = 0;

    virtual void releaseGlResources() = 0;
};
