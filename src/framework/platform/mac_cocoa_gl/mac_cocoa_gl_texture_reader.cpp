#include "mac_cocoa_gl_texture_reader.h"

#include "framework/platform/gl_types.h"
#include "framework/platform/presentation_events.h"
#include "mac_iosurface_texture_slots.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>

#if defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>
#endif

#if defined(Q_OS_MACOS)
struct MacCocoaGlTextureReader::ImportedSlot final
{
    IOSurfaceRef ioSurface = nullptr;
    QSize size;
    quint64 generation = 0;
    GLuint textureId = 0U;
    GLenum textureTarget = GL_TEXTURE_RECTANGLE_ARB;
};

namespace
{
QString cglErrorToString(CGLError error)
{
    const char *description = CGLErrorString(error);
    if (description == nullptr) {
        return QStringLiteral("CGLError(%1)").arg(int(error));
    }
    return QString::fromLatin1(description);
}
}
#endif

MacCocoaGlTextureReader::MacCocoaGlTextureReader(const std::shared_ptr<MacIoSurfaceTextureSlots> &slotPool,
                                                 PlatformPresentationEvents *presentationEvents)
    : m_slotPool(slotPool)
    , m_presentationEvents(presentationEvents)
{
}

MacCocoaGlTextureReader::~MacCocoaGlTextureReader()
{
    detach();
}

bool MacCocoaGlTextureReader::attachToCurrentContext(QString *error)
{
    m_context = QOpenGLContext::currentContext();
    m_gl = m_context ? m_context->functions() : nullptr;
    if (!m_context || !m_gl) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlTextureReader requires a current QOpenGLContext.");
        }
        return false;
    }
#if defined(Q_OS_MACOS)
    m_importedSlots.resize(m_slotPool ? m_slotPool->slotCount() : 0);
#endif
    return true;
}

bool MacCocoaGlTextureReader::acquire(const TextureTicket &ticket, TextureLease *lease, QString *error)
{
#if !defined(Q_OS_MACOS)
    Q_UNUSED(ticket);
    Q_UNUSED(lease);
    if (error) {
        *error = QStringLiteral("MacCocoaGlTextureReader is only available on macOS.");
    }
    return false;
#else
    if (lease == nullptr || !m_slotPool || !m_gl) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlTextureReader is not attached or the lease output is invalid.");
        }
        return false;
    }

    MacIoSurfaceSlotInfo slotInfo;
    if (!m_slotPool->acquireReadyTexture(ticket, &slotInfo)) {
        if (error) {
            *error = QStringLiteral("The requested texture ticket is no longer ready for reading.");
        }
        return false;
    }

    if (!ensureImportedSlot(ticket.slotIndex, slotInfo, error)) {
        m_slotPool->releaseReadingSlot(ticket.slotIndex);
        return false;
    }

    ImportedSlot *slot = m_importedSlots[ticket.slotIndex].get();
    if (slot == nullptr || slot->textureId == 0U) {
        m_slotPool->releaseReadingSlot(ticket.slotIndex);
        if (error) {
            *error = QStringLiteral("MacCocoaGlTextureReader slot %1 import is unavailable.").arg(ticket.slotIndex);
        }
        return false;
    }

    lease->textureId = slot->textureId;
    lease->textureTarget = slot->textureTarget;
    lease->size = ticket.size;
    return true;
#endif
}

void MacCocoaGlTextureReader::release(const TextureLease &lease)
{
#if defined(Q_OS_MACOS)
    const int slotIndex = m_slotIndexByTextureId.value(lease.textureId, -1);
    if (slotIndex >= 0 && m_slotPool) {
        m_gl->glBindTexture(lease.textureTarget, 0);
        m_slotPool->releaseReadingSlot(slotIndex);
    }
#else
    Q_UNUSED(lease);
#endif
    if (m_presentationEvents) {
        emit m_presentationEvents->publishCapacityAvailable();
    }
}

void MacCocoaGlTextureReader::detach()
{
#if defined(Q_OS_MACOS)
    for (int i = 0; i < m_importedSlots.size(); ++i) {
        destroyImportedSlot(i);
    }
    m_importedSlots.clear();
    m_slotIndexByTextureId.clear();
#endif
    m_context = nullptr;
    m_gl = nullptr;
}

bool MacCocoaGlTextureReader::ensureImportedSlot(int slotIndex, const MacIoSurfaceSlotInfo &slotInfo, QString *error)
{
#if !defined(Q_OS_MACOS)
    Q_UNUSED(slotIndex);
    Q_UNUSED(slotInfo);
    Q_UNUSED(error);
    return false;
#else
    if (!m_gl || slotIndex < 0 || slotIndex >= m_importedSlots.size()) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlTextureReader import prerequisites are incomplete.");
        }
        return false;
    }

    if (!m_importedSlots[slotIndex]) {
        m_importedSlots[slotIndex] = std::make_unique<ImportedSlot>();
    }

    ImportedSlot &slot = *m_importedSlots[slotIndex];
    if (slot.textureId != 0U
        && slot.generation == slotInfo.generation
        && slot.ioSurface == slotInfo.ioSurface
        && slot.size == slotInfo.size
        && slot.textureTarget == slotInfo.textureTarget) {
        return true;
    }

    destroyImportedSlot(slotIndex);
    if (!m_importedSlots[slotIndex]) {
        m_importedSlots[slotIndex] = std::make_unique<ImportedSlot>();
    }

    ImportedSlot &newSlot = *m_importedSlots[slotIndex];
    if (slotInfo.ioSurface == nullptr || !slotInfo.size.isValid()) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlTextureReader slot %1 is unavailable.").arg(slotIndex);
        }
        return false;
    }

    m_gl->glGenTextures(1, &newSlot.textureId);
    if (newSlot.textureId == 0U) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlTextureReader failed to allocate imported texture for slot %1.")
                         .arg(slotIndex);
        }
        return false;
    }

    newSlot.textureTarget = slotInfo.textureTarget;
    m_gl->glBindTexture(newSlot.textureTarget, newSlot.textureId);
    m_gl->glTexParameteri(newSlot.textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(newSlot.textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(newSlot.textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(newSlot.textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    CGLContextObj cglContext = CGLGetCurrentContext();
    const CGLError bindError = CGLTexImageIOSurface2D(cglContext,
                                                      newSlot.textureTarget,
                                                      GL_RGBA,
                                                      slotInfo.size.width(),
                                                      slotInfo.size.height(),
                                                      GL_BGRA,
                                                      GL_UNSIGNED_INT_8_8_8_8_REV,
                                                      slotInfo.ioSurface,
                                                      0);
    m_gl->glBindTexture(newSlot.textureTarget, 0);
    if (bindError != kCGLNoError) {
        m_gl->glDeleteTextures(1, &newSlot.textureId);
        newSlot.textureId = 0U;
        if (error) {
            *error = QStringLiteral("CGLTexImageIOSurface2D import failed for slot %1: %2")
                         .arg(slotIndex)
                         .arg(cglErrorToString(bindError));
        }
        return false;
    }

    newSlot.ioSurface = slotInfo.ioSurface;
    CFRetain(newSlot.ioSurface);
    newSlot.size = slotInfo.size;
    newSlot.generation = slotInfo.generation;
    m_slotIndexByTextureId.insert(newSlot.textureId, slotIndex);
    return true;
#endif
}

void MacCocoaGlTextureReader::destroyImportedSlot(int slotIndex)
{
#if defined(Q_OS_MACOS)
    if (!m_gl || slotIndex < 0 || slotIndex >= m_importedSlots.size() || !m_importedSlots[slotIndex]) {
        return;
    }

    ImportedSlot &slot = *m_importedSlots[slotIndex];
    if (slot.textureId != 0U) {
        m_slotIndexByTextureId.remove(slot.textureId);
        m_gl->glDeleteTextures(1, &slot.textureId);
        slot.textureId = 0U;
    }
    if (slot.ioSurface != nullptr) {
        CFRelease(slot.ioSurface);
        slot.ioSurface = nullptr;
    }
    slot.size = QSize();
    slot.generation = 0;
    slot.textureTarget = GL_TEXTURE_RECTANGLE_ARB;
#else
    Q_UNUSED(slotIndex);
#endif
}
