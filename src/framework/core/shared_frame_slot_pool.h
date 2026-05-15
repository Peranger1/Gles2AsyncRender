#pragma once

#include "work_types.h"

class IFrameReader
{
public:
    virtual ~IFrameReader() = default;

    virtual int slotCount() const = 0;
    virtual bool querySlot(int slotIndex, FrameSlotInfo *slotInfo) const = 0;
    virtual bool consumePendingFrame(const FrameTicket &ticket, FrameSlotInfo *slotInfo) = 0;
    virtual void releasePendingSlot(int slotIndex) = 0;
};

class IFrameWriter : public IFrameReader
{
public:
    ~IFrameWriter() override = default;

    virtual void reset() = 0;
    virtual void updateSlot(int slotIndex, quintptr sharedHandle, const QSize &size, quint64 generation) = 0;
    virtual bool tryAcquireRenderSlot(int *slotIndex) = 0;
    virtual void abandonRenderSlot(int slotIndex) = 0;
    virtual bool submitRenderedFrame(int slotIndex, quint64 frameIndex, FrameTicket *ticket) = 0;
};
