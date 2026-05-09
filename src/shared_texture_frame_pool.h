#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QSize>
#include <QString>
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

    ~SharedTextureFramePool()
    {
        reset();
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
            if (slot.state == SlotState::Free && !slot.inComposeRead && slot.textureId != 0U) {
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

    void updateSlotFence(int slotIndex, GLsync writeFence, bool syncSupported)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return;
        }

        Slot &slot = m_slots[slotIndex];
        slot.writeFence = writeFence;
        slot.needsFenceWait = syncSupported && writeFence != nullptr;
        slot.syncSupported = syncSupported;
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

    bool consumePendingFrame(int slotIndex,
                             QOpenGLContext *displayContext,
                             int *retiredSlot,
                             SharedTextureFrame *frame,
                             QString *error)
    {
        if (!isValidSlotIndex(slotIndex) || displayContext == nullptr) {
            if (error) {
                *error = QStringLiteral("Pending frame promotion prerequisites are incomplete.");
            }
            return false;
        }

        GLsync waitFence = nullptr;
        bool syncSupported = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_pendingSlot != slotIndex) {
                return false;
            }

            Slot &pendingSlot = m_slots[slotIndex];
            if (pendingSlot.state != SlotState::Pending) {
                return false;
            }

            waitFence = pendingSlot.writeFence;
            syncSupported = pendingSlot.syncSupported;
        }

        QOpenGLExtraFunctions *extra = displayContext->extraFunctions();
        if (waitFence != nullptr && syncSupported) {
            if (extra == nullptr) {
                if (error) {
                    *error = QStringLiteral("Display context does not expose QOpenGLExtraFunctions.");
                }
                return false;
            }

            QElapsedTimer waitTimer;
            waitTimer.start();
            for (;;) {
                const GLenum waitResult = extra->glClientWaitSync(waitFence, 0, 1000000ULL);
                if (waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED) {
                    break;
                }

                if (waitResult == GL_WAIT_FAILED) {
                    if (error) {
                        *error = QStringLiteral("glClientWaitSync failed while consuming the pending slot.");
                    }
                    return false;
                }

                if (waitTimer.elapsed() > 5000) {
                    if (error) {
                        *error = QStringLiteral("Timed out waiting for the pending slot fence.");
                    }
                    return false;
                }
            }
        }

        QMutexLocker locker(&m_mutex);
        if (m_pendingSlot != slotIndex) {
            return false;
        }

        Slot &nextFront = m_slots[slotIndex];
        if (nextFront.state != SlotState::Pending) {
            return false;
        }

        if (waitFence != nullptr && nextFront.writeFence == waitFence) {
            nextFront.writeFence = nullptr;
            nextFront.needsFenceWait = false;
            nextFront.syncSupported = false;
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
        nextFront.inComposeRead = false;
        m_frontSlot = slotIndex;
        m_pendingSlot = -1;

        if (frame) {
            frame->slotIndex = slotIndex;
            frame->textureId = nextFront.textureId;
            frame->size = nextFront.size;
            frame->frameIndex = nextFront.frameIndex;
        }
        if (retiredSlot) {
            *retiredSlot = oldFront;
        }

        if (waitFence != nullptr && syncSupported) {
            extra->glDeleteSync(waitFence);
        }
        return true;
    }

    void markFrontSlotComposing(bool inComposeRead)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(m_frontSlot)) {
            return;
        }

        m_slots[m_frontSlot].inComposeRead = inComposeRead;
    }

    void releaseRetiredSlot(int slotIndex)
    {
        QMutexLocker locker(&m_mutex);
        if (!isValidSlotIndex(slotIndex)) {
            return;
        }

        releaseRetiredSlotLocked(slotIndex);
    }

    void releaseSlotFence(int slotIndex, QOpenGLContext *context)
    {
        if (!isValidSlotIndex(slotIndex) || context == nullptr) {
            return;
        }

        GLsync fence = nullptr;
        {
            QMutexLocker locker(&m_mutex);
            Slot &slot = m_slots[slotIndex];
            fence = slot.writeFence;
            slot.writeFence = nullptr;
            slot.needsFenceWait = false;
            slot.syncSupported = false;
        }

        if (fence == nullptr) {
            return;
        }

        if (QOpenGLExtraFunctions *extra = context->extraFunctions()) {
            extra->glDeleteSync(fence);
        }
    }

    void reset()
    {
        QMutexLocker locker(&m_mutex);

        for (Slot &slot : m_slots) {
            slot.state = SlotState::Free;
            slot.size = QSize();
            slot.frameIndex = 0U;
            slot.writeFence = nullptr;
            slot.needsFenceWait = false;
            slot.syncSupported = false;
            slot.inComposeRead = false;
        }

        m_frontSlot = -1;
        m_pendingSlot = -1;
    }

    void releaseAllFences(QOpenGLContext *context)
    {
        if (context == nullptr) {
            return;
        }

        for (int slotIndex = 0; slotIndex < m_slots.size(); ++slotIndex) {
            releaseSlotFence(slotIndex, context);
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
        quint32 textureId = 0U;
        QSize size;
        quint64 frameIndex = 0U;
        GLsync writeFence = nullptr;
        bool needsFenceWait = false;
        bool syncSupported = false;
        bool inComposeRead = false;
    };

    bool isValidSlotIndex(int slotIndex) const
    {
        return slotIndex >= 0 && slotIndex < m_slots.size();
    }

    void releaseRetiredSlotLocked(int slotIndex)
    {
        Slot &slot = m_slots[slotIndex];
        if (slot.state == SlotState::Retiring && !slot.inComposeRead) {
            slot.state = SlotState::Free;
            slot.size = QSize();
            slot.frameIndex = 0U;
        }
    }

    mutable QMutex m_mutex;
    QVector<Slot> m_slots;
    int m_frontSlot = -1;
    int m_pendingSlot = -1;
};

Q_DECLARE_METATYPE(SharedTextureFramePool *)
