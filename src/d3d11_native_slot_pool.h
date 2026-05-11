#pragma once

#include "framework/core/async_render_types.h"
#include "framework/core/shared_frame_slot_pool.h"

#include <QMutex>
#include <QSize>
#include <QVector>

using D3D11NativeFrame = PublishedFrame;

class D3D11NativeSlotPool final : public ISharedFrameSlotPool
{
public:
    explicit D3D11NativeSlotPool(int slotCount = 3)
        : m_slots(qMax(2, slotCount))
    {
    }

    int slotCount() const override
    {
        return m_slots.size();
    }

    void reset() override
    {
        QMutexLocker locker(&m_mutex);
        for (Slot &slot : m_slots) {
            slot.state = SlotState::Free;
            slot.sharedHandle = 0;
            slot.size = QSize();
            slot.generation = 0;
            slot.frameIndex = 0;
        }
        m_pendingSlot = -1;
    }

    void updateSlot(int slotIndex, quintptr sharedHandle, const QSize &size, quint64 generation) override
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return;
        }

        Slot &slot = m_slots[slotIndex];
        slot.sharedHandle = sharedHandle;
        slot.size = size;
        slot.generation = generation;
    }

    bool querySlot(int slotIndex, D3D11NativeFrame *frame) const override
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || frame == nullptr) {
            return false;
        }

        const Slot &slot = m_slots[slotIndex];
        if (slot.sharedHandle == 0) {
            return false;
        }

        frame->slotIndex = slotIndex;
        frame->sharedHandle = slot.sharedHandle;
        frame->size = slot.size;
        frame->generation = slot.generation;
        frame->frameIndex = slot.frameIndex;
        return true;
    }

    bool tryAcquireRenderSlot(int *slotIndex) override
    {
        QMutexLocker locker(&m_mutex);
        if (m_pendingSlot != -1) {
            return false;
        }

        for (int i = 0; i < m_slots.size(); ++i) {
            Slot &slot = m_slots[i];
            if (slot.state == SlotState::Free) {
                slot.state = SlotState::Rendering;
                if (slotIndex) {
                    *slotIndex = i;
                }
                return true;
            }
        }
        return false;
    }

    void abandonRenderSlot(int slotIndex) override
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return;
        }

        Slot &slot = m_slots[slotIndex];
        if (slot.state == SlotState::Rendering) {
            slot.state = SlotState::Free;
        }
    }

    bool submitRenderedFrame(int slotIndex, quint64 frameIndex, D3D11NativeFrame *frame) override
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || frame == nullptr) {
            return false;
        }

        Slot &slot = m_slots[slotIndex];
        if (slot.state != SlotState::Rendering || slot.sharedHandle == 0) {
            return false;
        }

        if (m_pendingSlot != -1 && m_pendingSlot != slotIndex) {
            return false;
        }

        slot.state = SlotState::Pending;
        slot.frameIndex = frameIndex;
        m_pendingSlot = slotIndex;

        frame->slotIndex = slotIndex;
        frame->sharedHandle = slot.sharedHandle;
        frame->size = slot.size;
        frame->generation = slot.generation;
        frame->frameIndex = slot.frameIndex;
        return true;
    }

    bool consumePendingFrame(int slotIndex, D3D11NativeFrame *frame) override
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || m_pendingSlot != slotIndex || frame == nullptr) {
            return false;
        }

        const Slot &slot = m_slots[slotIndex];
        if (slot.state != SlotState::Pending) {
            return false;
        }

        frame->slotIndex = slotIndex;
        frame->sharedHandle = slot.sharedHandle;
        frame->size = slot.size;
        frame->generation = slot.generation;
        frame->frameIndex = slot.frameIndex;
        return true;
    }

    void releasePendingSlot(int slotIndex) override
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return;
        }

        Slot &slot = m_slots[slotIndex];
        if (slot.state == SlotState::Pending && m_pendingSlot == slotIndex) {
            slot.state = SlotState::Free;
            m_pendingSlot = -1;
        }
    }

private:
    enum class SlotState
    {
        Free,
        Rendering,
        Pending
    };

    struct Slot final
    {
        SlotState state = SlotState::Free;
        quintptr sharedHandle = 0;
        QSize size;
        quint64 generation = 0;
        quint64 frameIndex = 0;
    };

    bool isValidSlotIndex(int slotIndex) const
    {
        return slotIndex >= 0 && slotIndex < m_slots.size();
    }

    mutable QMutex m_mutex;
    QVector<Slot> m_slots;
    int m_pendingSlot = -1;
};
