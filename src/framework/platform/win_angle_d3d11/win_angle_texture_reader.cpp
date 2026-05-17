#include "win_angle_texture_reader.h"

#include "framework/backend/win_angle_d3d11/qt_angle_egl_tools.h"
#include "framework/platform/presentation_events.h"
#include "d3d11_shared_texture_slots.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <QtANGLE/EGL/eglext.h>

WinAngleTextureReader::WinAngleTextureReader(const std::shared_ptr<D3D11SharedTextureSlots> &slotPool,
                                             PlatformPresentationEvents *presentationEvents)
    : m_slotPool(slotPool)
    , m_presentationEvents(presentationEvents)
{
}

WinAngleTextureReader::~WinAngleTextureReader()
{
    detach();
}

bool WinAngleTextureReader::attachToCurrentContext(QString *error)
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr || context->functions() == nullptr) {
        if (error) {
            *error = QStringLiteral("WinAngleTextureReader requires a current QOpenGLContext.");
        }
        return false;
    }

    m_context = context;
    m_gl = context->functions();
    QStringList probeLog;
    m_ownedEglApi = std::make_unique<QtAngleEglTools::ResolvedEglApi>(QtAngleEglTools::resolveEglApi(context, &probeLog));
    m_eglApi = m_ownedEglApi.get();
    if (!m_eglApi->supportsD3DTextureImport()) {
        if (error) {
            *error = QStringLiteral("WinAngleTextureReader could not resolve required Qt EGL import functions.");
        }
        return false;
    }

    m_eglDisplay = QtAngleEglTools::queryDisplay(context, *m_eglApi, &probeLog);
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        if (error) {
            *error = QStringLiteral("WinAngleTextureReader could not resolve the Qt-owned EGLDisplay.");
        }
        return false;
    }

    m_eglConfig = QtAngleEglTools::queryConfig(context, &probeLog);
    m_importedSlots.resize(m_slotPool ? m_slotPool->slotCount() : 0);
    return true;
}

bool WinAngleTextureReader::acquire(const TextureTicket &ticket, TextureLease *lease, QString *error)
{
    if (lease == nullptr || !m_slotPool || !m_gl || !m_eglApi) {
        if (error) {
            *error = QStringLiteral("WinAngleTextureReader is not attached or the lease output is invalid.");
        }
        return false;
    }

    D3D11SharedTextureSlotInfo slotInfo;
    if (!m_slotPool->acquireReadyTexture(ticket, &slotInfo)) {
        if (error) {
            *error = QStringLiteral("The requested texture ticket is no longer ready for reading.");
        }
        return false;
    }

    if (!ensureImportedSlot(ticket.slotIndex, ticket.size, error)) {
        m_slotPool->releaseReadingSlot(ticket.slotIndex);
        return false;
    }

    ImportedSlot &slot = m_importedSlots[ticket.slotIndex];
    const HRESULT acquireHr = slot.keyedMutex->AcquireSync(1, 5);
    if (acquireHr != S_OK) {
        m_slotPool->releaseReadingSlot(ticket.slotIndex);
        if (error) {
            *error = QStringLiteral("AcquireSync(slot %1, key 1) failed: 0x%2")
                         .arg(ticket.slotIndex)
                         .arg(static_cast<unsigned int>(acquireHr), 0, 16);
        }
        return false;
    }

    m_gl->glBindTexture(GL_TEXTURE_2D, slot.textureId);
    if (m_eglApi->bindTexImage(m_eglDisplay, slot.surface, EGL_BACK_BUFFER) != EGL_TRUE) {
        m_gl->glBindTexture(GL_TEXTURE_2D, 0);
        slot.keyedMutex->ReleaseSync(0);
        m_slotPool->releaseReadingSlot(ticket.slotIndex);
        if (error) {
            *error = QStringLiteral("eglBindTexImage(slot %1) failed with EGL error %2")
                         .arg(ticket.slotIndex)
                         .arg(QtAngleEglTools::eglErrorToString(m_eglApi->getError()));
        }
        return false;
    }

    slot.boundForRead = true;
    lease->textureId = slot.textureId;
    lease->size = ticket.size;
    return true;
}

void WinAngleTextureReader::release(const TextureLease &lease)
{
    if (!m_slotIndexByTextureId.contains(lease.textureId) || !m_gl || !m_eglApi || !m_slotPool) {
        return;
    }

    const int slotIndex = m_slotIndexByTextureId.value(lease.textureId, -1);
    if (slotIndex < 0 || slotIndex >= m_importedSlots.size()) {
        return;
    }

    ImportedSlot &slot = m_importedSlots[slotIndex];
    if (slot.boundForRead) {
        m_eglApi->releaseTexImage(m_eglDisplay, slot.surface, EGL_BACK_BUFFER);
        slot.boundForRead = false;
        slot.keyedMutex->ReleaseSync(0);
    }

    m_gl->glBindTexture(GL_TEXTURE_2D, 0);
    m_slotPool->releaseReadingSlot(slotIndex);
    if (m_presentationEvents) {
        emit m_presentationEvents->publishCapacityAvailable();
    }
}

void WinAngleTextureReader::detach()
{
    if (!m_gl) {
        return;
    }

    for (int i = 0; i < m_importedSlots.size(); ++i) {
        destroyImportedSlot(i);
    }
    m_slotIndexByTextureId.clear();
    m_importedSlots.clear();
    m_ownedEglApi.reset();
    m_eglApi = nullptr;
    m_eglDisplay = EGL_NO_DISPLAY;
    m_eglConfig = nullptr;
    m_context = nullptr;
    m_gl = nullptr;
}

bool WinAngleTextureReader::ensureImportedSlot(int slotIndex, const QSize &expectedSize, QString *error)
{
    if (!m_slotPool || !m_eglApi || slotIndex < 0 || slotIndex >= m_importedSlots.size()) {
        if (error) {
            *error = QStringLiteral("WinAngleTextureReader import prerequisites are incomplete.");
        }
        return false;
    }

    D3D11SharedTextureSlotInfo slotInfo;
    if (!m_slotPool->querySlot(slotIndex, &slotInfo)) {
        if (error) {
            *error = QStringLiteral("WinAngleTextureReader slot %1 is unavailable.").arg(slotIndex);
        }
        return false;
    }

    ImportedSlot &slot = m_importedSlots[slotIndex];
    if (slot.surface != EGL_NO_SURFACE
        && slot.generation == slotInfo.generation
        && slot.sharedHandle == slotInfo.sharedHandle
        && slot.size == expectedSize) {
        return true;
    }

    destroyImportedSlot(slotIndex);

    const EGLint surfaceAttributes[] = {
        EGL_WIDTH, expectedSize.width(),
        EGL_HEIGHT, expectedSize.height(),
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_NONE
    };

    slot.surface = m_eglApi->createPbufferFromClientBuffer(
        m_eglDisplay,
        EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
        reinterpret_cast<EGLClientBuffer>(slotInfo.sharedHandle),
        m_eglConfig,
        surfaceAttributes);
    if (slot.surface == EGL_NO_SURFACE) {
        if (error) {
            *error = QStringLiteral("eglCreatePbufferFromClientBuffer(slot %1) failed with EGL error %2")
                         .arg(slotIndex)
                         .arg(QtAngleEglTools::eglErrorToString(m_eglApi->getError()));
        }
        return false;
    }

    void *keyedMutexPtr = nullptr;
    if (m_eglApi->querySurfacePointerANGLE(m_eglDisplay, slot.surface, EGL_DXGI_KEYED_MUTEX_ANGLE, &keyedMutexPtr) != EGL_TRUE
        || keyedMutexPtr == nullptr) {
        if (error) {
            *error = QStringLiteral("eglQuerySurfacePointerANGLE(EGL_DXGI_KEYED_MUTEX_ANGLE) failed for slot %1.")
                         .arg(slotIndex);
        }
        destroyImportedSlot(slotIndex);
        return false;
    }

    IDXGIKeyedMutex *rawMutex = reinterpret_cast<IDXGIKeyedMutex *>(keyedMutexPtr);
    rawMutex->AddRef();
    slot.keyedMutex.Attach(rawMutex);

    m_gl->glGenTextures(1, &slot.textureId);
    m_gl->glBindTexture(GL_TEXTURE_2D, slot.textureId);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_gl->glBindTexture(GL_TEXTURE_2D, 0);

    slot.generation = slotInfo.generation;
    slot.sharedHandle = slotInfo.sharedHandle;
    slot.size = slotInfo.size;
    m_slotIndexByTextureId.insert(slot.textureId, slotIndex);
    return true;
}

void WinAngleTextureReader::destroyImportedSlot(int slotIndex)
{
    if (!m_gl || slotIndex < 0 || slotIndex >= m_importedSlots.size()) {
        return;
    }

    ImportedSlot &slot = m_importedSlots[slotIndex];
    if (slot.boundForRead && m_eglApi && slot.surface != EGL_NO_SURFACE) {
        m_eglApi->releaseTexImage(m_eglDisplay, slot.surface, EGL_BACK_BUFFER);
        slot.boundForRead = false;
        if (slot.keyedMutex) {
            slot.keyedMutex->ReleaseSync(0);
        }
    }
    if (slot.textureId != 0U) {
        m_slotIndexByTextureId.remove(slot.textureId);
        m_gl->glDeleteTextures(1, &slot.textureId);
        slot.textureId = 0U;
    }
    slot.keyedMutex.Reset();
    if (slot.surface != EGL_NO_SURFACE && m_eglApi) {
        m_eglApi->destroySurface(m_eglDisplay, slot.surface);
        slot.surface = EGL_NO_SURFACE;
    }
    slot.sharedHandle = 0;
    slot.generation = 0;
    slot.size = QSize();
}
