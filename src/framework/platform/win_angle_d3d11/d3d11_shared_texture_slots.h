#pragma once

#include "framework/platform/texture_types.h"

#include <QMutex>
#include <QQueue>
#include <QSize>
#include <QVector>
#include <QtGlobal>

struct D3D11SharedTextureSlotInfo final
{
    quintptr sharedHandle = 0;
    QSize size;
    quint64 generation = 0;
};

class D3D11SharedTextureSlots final
{
public:
    explicit D3D11SharedTextureSlots(int slotCount = 3)
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
        m_readySlots.clear();
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

    bool querySlot(int slotIndex, D3D11SharedTextureSlotInfo *slotInfo) const
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || slotInfo == nullptr) {
            return false;
        }

        const Slot &slot = m_slots[slotIndex];
        if (slot.sharedHandle == 0) {
            return false;
        }

        slotInfo->sharedHandle = slot.sharedHandle;
        slotInfo->size = slot.size;
        slotInfo->generation = slot.generation;
        return true;
    }

    bool tryAcquireRenderSlot(int *slotIndex)
    {
        QMutexLocker locker(&m_mutex);
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

        if (!m_readySlots.isEmpty()) {
            const int recycledSlotIndex = m_readySlots.dequeue();
            if (!isValidSlotIndex(recycledSlotIndex)) {
                return false;
            }

            Slot &slot = m_slots[recycledSlotIndex];
            if (slot.state != SlotState::Ready) {
                return false;
            }

            slot.state = SlotState::Rendering;
            if (slotIndex) {
                *slotIndex = recycledSlotIndex;
            }
            return true;
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

    bool submitRenderedTexture(int slotIndex, quint64 frameIndex, quint64 outputRevision, TextureTicket *ticket)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || ticket == nullptr) {
            return false;
        }

        Slot &slot = m_slots[slotIndex];
        if (slot.state != SlotState::Rendering || slot.sharedHandle == 0) {
            return false;
        }

        slot.state = SlotState::Ready;
        slot.frameIndex = frameIndex;
        m_readySlots.enqueue(slotIndex);

        ticket->slotIndex = slotIndex;
        ticket->generation = slot.generation;
        ticket->frameIndex = slot.frameIndex;
        ticket->outputRevision = outputRevision;
        ticket->size = slot.size;
        return true;
    }

    bool acquireReadyTexture(const TextureTicket &ticket, D3D11SharedTextureSlotInfo *slotInfo)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(ticket.slotIndex) || slotInfo == nullptr) {
            return false;
        }

        Slot &slot = m_slots[ticket.slotIndex];
        if (slot.state != SlotState::Ready) {
            return false;
        }
        if (slot.generation != ticket.generation || slot.frameIndex != ticket.frameIndex) {
            return false;
        }

        slotInfo->sharedHandle = slot.sharedHandle;
        slotInfo->size = slot.size;
        slotInfo->generation = slot.generation;
        slot.state = SlotState::Reading;
        removeReadySlot(ticket.slotIndex);
        return true;
    }

    void releaseReadingSlot(int slotIndex)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return;
        }

        Slot &slot = m_slots[slotIndex];
        if (slot.state == SlotState::Reading) {
            slot.state = SlotState::Free;
        }
    }

private:
    enum class SlotState
    {
        Free,
        Rendering,
        Ready,
        Reading
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

    void removeReadySlot(int slotIndex)
    {
        for (int i = 0; i < m_readySlots.size(); ++i) {
            if (m_readySlots[i] == slotIndex) {
                m_readySlots.removeAt(i);
                return;
            }
        }
    }

    mutable QMutex m_mutex;
    QVector<Slot> m_slots;
    QQueue<int> m_readySlots;
};
