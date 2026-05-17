#pragma once

#include "framework/platform/texture_types.h"

#include <QMutex>
#include <QQueue>
#include <QSize>
#include <QVector>
#include <QtGlobal>

#if defined(Q_OS_MACOS)
#include <IOSurface/IOSurface.h>
#endif

struct MacIoSurfaceSlotInfo final
{
#if defined(Q_OS_MACOS)
    IOSurfaceRef ioSurface = nullptr;
#endif
    QSize size;
    quint64 generation = 0;
    GLenum textureTarget = GL_TEXTURE_2D;
};

class MacIoSurfaceTextureSlots final
{
public:
    explicit MacIoSurfaceTextureSlots(int slotCount = 3)
        : m_slots(qMax(2, slotCount))
    {
    }

    ~MacIoSurfaceTextureSlots()
    {
        reset();
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
#if defined(Q_OS_MACOS)
            if (slot.ioSurface != nullptr) {
                CFRelease(slot.ioSurface);
            }
            slot.ioSurface = nullptr;
#endif
            slot.size = QSize();
            slot.generation = 0;
            slot.frameIndex = 0;
            slot.textureTarget = GL_TEXTURE_2D;
        }
        m_readySlots.clear();
    }

#if defined(Q_OS_MACOS)
    void updateSlot(int slotIndex, IOSurfaceRef ioSurface, const QSize &size, quint64 generation, GLenum textureTarget)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return;
        }

        Slot &slot = m_slots[slotIndex];
        if (slot.ioSurface == ioSurface) {
            slot.size = size;
            slot.generation = generation;
            slot.textureTarget = textureTarget;
            return;
        }

        if (ioSurface != nullptr) {
            CFRetain(ioSurface);
        }
        if (slot.ioSurface != nullptr) {
            CFRelease(slot.ioSurface);
        }
        slot.ioSurface = ioSurface;
        slot.size = size;
        slot.generation = generation;
        slot.textureTarget = textureTarget;
    }
#endif

    bool querySlot(int slotIndex, MacIoSurfaceSlotInfo *slotInfo) const
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex) || slotInfo == nullptr) {
            return false;
        }

        const Slot &slot = m_slots[slotIndex];
#if defined(Q_OS_MACOS)
        if (slot.ioSurface == nullptr) {
            return false;
        }
        slotInfo->ioSurface = slot.ioSurface;
#endif
        slotInfo->size = slot.size;
        slotInfo->generation = slot.generation;
        slotInfo->textureTarget = slot.textureTarget;
        return slot.size.isValid();
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
        if (slot.state != SlotState::Rendering || !slot.size.isValid()) {
            return false;
        }
#if defined(Q_OS_MACOS)
        if (slot.ioSurface == nullptr) {
            return false;
        }
#endif

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

    bool acquireReadyTexture(const TextureTicket &ticket, MacIoSurfaceSlotInfo *slotInfo)
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

#if defined(Q_OS_MACOS)
        slotInfo->ioSurface = slot.ioSurface;
#endif
        slotInfo->size = slot.size;
        slotInfo->generation = slot.generation;
        slotInfo->textureTarget = slot.textureTarget;
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
#if defined(Q_OS_MACOS)
        IOSurfaceRef ioSurface = nullptr;
#endif
        QSize size;
        quint64 generation = 0;
        quint64 frameIndex = 0;
        GLenum textureTarget = GL_TEXTURE_2D;
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
