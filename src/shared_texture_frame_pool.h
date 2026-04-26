#pragma once

#include <QMutex>
#include <QSize>
#include <QVector>

struct SharedTextureFrame final
{
    int slotIndex = -1;
    quint32 textureId = 0U;
    QSize size;
    quint64 frameIndex = 0U;
};

class SharedTextureFramePool final
{
public:
    explicit SharedTextureFramePool(int slotCount)
        : m_slots(slotCount)
    {
    }

    int slotCount() const
    {
        return m_slots.size();
    }

    void registerTexture(int slotIndex, quint32 textureId)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return;
        }

        m_slots[slotIndex].textureId = textureId;
    }

    quint32 textureIdForSlot(int slotIndex) const
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return 0U;
        }

        return m_slots[slotIndex].textureId;
    }

    bool tryAcquireRenderSlot(int *slotIndex)
    {
        QMutexLocker locker(&m_mutex);

        if (m_pendingSlot != -1) {
            return false;
        }

        for (int i = 0; i < m_slots.size(); ++i) {
            Slot &slot = m_slots[i];
            if (slot.state == SlotState::Free && slot.textureId != 0U) {
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

    bool submitRenderedFrame(int slotIndex, const QSize &size, quint64 frameIndex, SharedTextureFrame *frame)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || m_pendingSlot != -1) {
            return false;
        }

        Slot &slot = m_slots[slotIndex];
        if (slot.state != SlotState::Rendering || slot.textureId == 0U) {
            return false;
        }

        slot.state = SlotState::Pending;
        slot.size = size;
        slot.frameIndex = frameIndex;
        m_pendingSlot = slotIndex;

        if (frame) {
            frame->slotIndex = slotIndex;
            frame->textureId = slot.textureId;
            frame->size = size;
            frame->frameIndex = frameIndex;
        }

        return true;
    }

    bool promotePendingFrame(int slotIndex, int *retiredSlot)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || m_pendingSlot != slotIndex) {
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
            slot.size = QSize();
            slot.frameIndex = 0U;
        }
    }

    void reset()
    {
        QMutexLocker locker(&m_mutex);

        for (Slot &slot : m_slots) {
            slot.state = SlotState::Free;
            slot.size = QSize();
            slot.frameIndex = 0U;
        }

        m_frontSlot = -1;
        m_pendingSlot = -1;
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
        quint32 textureId = 0U;
        QSize size;
        quint64 frameIndex = 0U;
    };

    bool isValidSlotIndex(int slotIndex) const
    {
        return slotIndex >= 0 && slotIndex < m_slots.size();
    }

    mutable QMutex m_mutex;
    QVector<Slot> m_slots;
    int m_frontSlot = -1;
    int m_pendingSlot = -1;
};

Q_DECLARE_METATYPE(SharedTextureFramePool *)
