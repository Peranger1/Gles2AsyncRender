#pragma once

#include "framework/core/shared_frame_slot_pool.h"

#include <QMutex>
#include <QSize>
#include <QVector>

class D3D11SharedSlotPool final : public IFrameWriter
{
public:
    explicit D3D11SharedSlotPool(int slotCount = 3)
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

    bool querySlot(int slotIndex, FrameSlotInfo *slotInfo) const override
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || slotInfo == nullptr) {
            return false;
        }

        const Slot &slot = m_slots[slotIndex];
        if (slot.sharedHandle == 0) {
            return false;
        }

        Q_UNUSED(slotIndex);
        slotInfo->sharedHandle = slot.sharedHandle;
        slotInfo->size = slot.size;
        slotInfo->generation = slot.generation;
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

    bool submitRenderedFrame(int slotIndex, quint64 frameIndex, FrameTicket *ticket) override
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || ticket == nullptr) {
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

        ticket->slotIndex = slotIndex;
        ticket->generation = slot.generation;
        ticket->frameIndex = slot.frameIndex;
        ticket->size = slot.size;
        return true;
    }

    bool consumePendingFrame(const FrameTicket &ticket, FrameSlotInfo *slotInfo) override
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(ticket.slotIndex) || m_pendingSlot != ticket.slotIndex || slotInfo == nullptr) {
            return false;
        }

        const Slot &slot = m_slots[ticket.slotIndex];
        if (slot.state != SlotState::Pending) {
            return false;
        }
        if (slot.generation != ticket.generation || slot.frameIndex != ticket.frameIndex) {
            return false;
        }

        slotInfo->sharedHandle = slot.sharedHandle;
        slotInfo->size = slot.size;
        slotInfo->generation = slot.generation;
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
