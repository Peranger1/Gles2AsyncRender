#pragma once

#include "async_render_types.h"

class ISharedFrameSlotPool
{
public:
    virtual ~ISharedFrameSlotPool() = default;

    virtual int slotCount() const = 0;
    virtual void reset() = 0;
    virtual void updateSlot(int slotIndex, quintptr sharedHandle, const QSize &size, quint64 generation) = 0;
    virtual bool querySlot(int slotIndex, PublishedFrame *frame) const = 0;
    virtual bool tryAcquireRenderSlot(int *slotIndex) = 0;
    virtual void abandonRenderSlot(int slotIndex) = 0;
    virtual bool submitRenderedFrame(int slotIndex, quint64 frameIndex, PublishedFrame *frame) = 0;
    virtual bool consumePendingFrame(int slotIndex, PublishedFrame *frame) = 0;
    virtual void releasePendingSlot(int slotIndex) = 0;
};
