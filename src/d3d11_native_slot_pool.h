#pragma once

#include <QMutex>
#include <QSize>
#include <QVector>

struct D3D11NativeFrame final
{
    int slotIndex = -1;
    quintptr sharedHandle = 0;
    QSize size;
    quint64 generation = 0;
    quint64 frameIndex = 0;
};

class D3D11NativeSlotPool final
{
public:
    explicit D3D11NativeSlotPool(int slotCount = 3)
        : m_slots(qMax(2, slotCount))
    {
    }

    int slotCount() const
    {
        return m_slots.size();
    }

    void reset()
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
        m_frontSlot = -1;
    }

    void updateSlot(int slotIndex, quintptr sharedHandle, const QSize &size, quint64 generation)
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

    bool querySlot(int slotIndex, D3D11NativeFrame *frame) const
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

    bool tryAcquireRenderSlot(int *slotIndex)
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

    void abandonRenderSlot(int slotIndex)
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

    bool submitRenderedFrame(int slotIndex, quint64 frameIndex, D3D11NativeFrame *frame)
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

    bool consumePendingFrame(int slotIndex, int *retiredSlot, D3D11NativeFrame *frame)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || m_pendingSlot != slotIndex || frame == nullptr) {
            return false;
        }

        Slot &nextFront = m_slots[slotIndex];
        if (nextFront.state != SlotState::Pending) {
            return false;
        }

        int oldFront = -1;
        if (m_frontSlot != -1 && m_frontSlot != slotIndex) {
            Slot &currentFront = m_slots[m_frontSlot];
            if (currentFront.state == SlotState::Front) {
                currentFront.state = SlotState::Retiring;
                oldFront = m_frontSlot;
            }
        }

        nextFront.state = SlotState::Front;
        m_frontSlot = slotIndex;
        m_pendingSlot = -1;

        frame->slotIndex = slotIndex;
        frame->sharedHandle = nextFront.sharedHandle;
        frame->size = nextFront.size;
        frame->generation = nextFront.generation;
        frame->frameIndex = nextFront.frameIndex;
        if (retiredSlot) {
            *retiredSlot = oldFront;
        }
        return true;
    }

    void releaseRetiredSlot(int slotIndex)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return;
        }

        Slot &slot = m_slots[slotIndex];
        if (slot.state == SlotState::Retiring) {
            slot.state = SlotState::Free;
        }
    }

private:
    enum class SlotState
    {
        Free,
        Rendering,
        Pending,
        Front,
        Retiring
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
    int m_frontSlot = -1;
};
