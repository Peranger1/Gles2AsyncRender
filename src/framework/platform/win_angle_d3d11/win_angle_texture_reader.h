#pragma once

#include "framework/platform/reader.h"

#include <QHash>
#include <QVector>

#include <QtANGLE/EGL/egl.h>

#include <dxgi.h>
#include <memory>
#include <wrl/client.h>

class D3D11SharedTextureSlots;
class QOpenGLContext;
class QOpenGLFunctions;

namespace QtAngleEglTools
{
struct ResolvedEglApi;
}

class WinAngleTextureReader final : public IReader
{
public:
    explicit WinAngleTextureReader(const std::shared_ptr<D3D11SharedTextureSlots> &slotPool);
    ~WinAngleTextureReader() override;

    bool attachToCurrentContext(QString *error) override;
    bool acquire(const TextureTicket &ticket, TextureLease *lease, QString *error) override;
    void release(const TextureLease &lease) override;
    void detach() override;

private:
    struct ImportedSlot final
    {
        quint64 generation = 0;
        quintptr sharedHandle = 0;
        QSize size;
        EGLSurface surface = EGL_NO_SURFACE;
        GLuint textureId = 0U;
        bool boundForRead = false;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyedMutex;
    };

    bool ensureImportedSlot(int slotIndex, const QSize &expectedSize, QString *error);
    void destroyImportedSlot(int slotIndex);

    std::shared_ptr<D3D11SharedTextureSlots> m_slotPool;
    QOpenGLContext *m_context = nullptr;
    QOpenGLFunctions *m_gl = nullptr;
    std::unique_ptr<QtAngleEglTools::ResolvedEglApi> m_ownedEglApi;
    QtAngleEglTools::ResolvedEglApi *m_eglApi = nullptr;
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLConfig m_eglConfig = nullptr;
    QVector<ImportedSlot> m_importedSlots;
    QHash<GLuint, int> m_slotIndexByTextureId;
};
