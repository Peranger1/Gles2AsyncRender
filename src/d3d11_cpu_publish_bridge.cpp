#include "d3d11_cpu_publish_bridge.h"

#include "angle_standalone_runtime.h"
#include "d3d11_native_slot_pool.h"

#include <QByteArray>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
QString hresultToString(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<unsigned int>(hr), 0, 16);
}

QString glErrorHex(GLenum error)
{
    return QStringLiteral("0x%1").arg(unsigned(error), 0, 16);
}

bool isAcquireTimeout(HRESULT hr)
{
    return hr == WAIT_TIMEOUT || hr == DXGI_ERROR_WAIT_TIMEOUT;
}

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

UINT rowPitchForWidth(int width)
{
    return UINT(qMax(1, width) * 4);
}
}

struct D3D11CpuPublishBridge::Impl final
{
    struct SlotResources final
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> keyedMutex;
        quintptr sharedHandle = 0;
        QSize size;
        quint64 generation = 0;
    };

    AngleStandaloneRuntime *runtime = nullptr;
    D3D11NativeSlotPool *slotPool = nullptr;
    QVector<SlotResources> slotResources;
    GLuint readbackFramebufferId = 0;
    QByteArray readbackBytes;
    QByteArray uploadBytes;

    bool initialize(AngleStandaloneRuntime *standaloneRuntime,
                    D3D11NativeSlotPool *slotPoolPtr,
                    QString *error)
    {
        runtime = standaloneRuntime;
        slotPool = slotPoolPtr;

        if (runtime == nullptr || slotPool == nullptr) {
            if (error) {
                *error = QStringLiteral("The CPU publish bridge requires both a runtime and a slot pool.");
            }
            return false;
        }

        const Gles2ProcTable &gl = runtime->procTable();
        if (!gl.isValid()) {
            if (error) {
                *error = QStringLiteral("The CPU publish bridge requires a valid standalone GLES2 proc table.");
            }
            return false;
        }

        gl.glGenFramebuffers(1, &readbackFramebufferId);
        if (readbackFramebufferId == 0U) {
            if (error) {
                *error = QStringLiteral("Failed to create the CPU publish readback framebuffer.");
            }
            return false;
        }

        slotResources.resize(slotPool->slotCount());
        return true;
    }

    bool publishToSlot(GLuint sourceTextureId, const QSize &sourceSize, int slotIndex, QString *error)
    {
        if (runtime == nullptr || slotPool == nullptr) {
            if (error) {
                *error = QStringLiteral("The CPU publish bridge is not initialized.");
            }
            return false;
        }
        if (sourceTextureId == 0U) {
            if (error) {
                *error = QStringLiteral("The CPU publish bridge requires a valid source texture.");
            }
            return false;
        }

        const QSize safeSize = sanitizedSize(sourceSize);
        if (!ensureSlotResources(slotIndex, safeSize, error)
            || !readPixels(sourceTextureId, safeSize, error)
            || !convertToBgra(safeSize, error)
            || !uploadPixels(slotIndex, safeSize, error)) {
            return false;
        }

        return true;
    }

    void releaseGlResources()
    {
        if (runtime == nullptr || readbackFramebufferId == 0U) {
            return;
        }

        const Gles2ProcTable &gl = runtime->procTable();
        gl.glDeleteFramebuffers(1, &readbackFramebufferId);
        readbackFramebufferId = 0U;
    }

    bool ensureSlotResources(int slotIndex, const QSize &size, QString *error)
    {
        if (slotIndex < 0 || slotIndex >= slotResources.size()) {
            if (error) {
                *error = QStringLiteral("The CPU publish bridge slot index is out of range.");
            }
            return false;
        }

        SlotResources &slot = slotResources[slotIndex];
        if (slot.texture && slot.sharedHandle != 0 && slot.size == size) {
            return true;
        }

        slot.texture.Reset();
        slot.keyedMutex.Reset();
        slot.sharedHandle = 0;
        slot.size = {};

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = UINT(size.width());
        desc.Height = UINT(size.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        HRESULT hr = runtime->d3d11Device()->CreateTexture2D(&desc, nullptr, &slot.texture);
        if (FAILED(hr) || !slot.texture) {
            if (error) {
                *error = QStringLiteral("Failed to create publish slot %1: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(hr));
            }
            return false;
        }

        hr = slot.texture.As(&slot.keyedMutex);
        if (FAILED(hr) || !slot.keyedMutex) {
            if (error) {
                *error = QStringLiteral("Failed to query IDXGIKeyedMutex for slot %1: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(hr));
            }
            return false;
        }

        ComPtr<IDXGIResource> resource;
        hr = slot.texture.As(&resource);
        if (FAILED(hr) || !resource) {
            if (error) {
                *error = QStringLiteral("Failed to query IDXGIResource for slot %1: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(hr));
            }
            return false;
        }

        HANDLE sharedHandle = nullptr;
        hr = resource->GetSharedHandle(&sharedHandle);
        if (FAILED(hr) || sharedHandle == nullptr) {
            if (error) {
                *error = QStringLiteral("Failed to get a shared handle for slot %1: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(hr));
            }
            return false;
        }

        slot.sharedHandle = quintptr(sharedHandle);
        slot.size = size;
        ++slot.generation;
        slotPool->updateSlot(slotIndex, slot.sharedHandle, slot.size, slot.generation);
        return true;
    }

    bool readPixels(GLuint sourceTextureId, const QSize &sourceSize, QString *error)
    {
        const Gles2ProcTable &gl = runtime->procTable();
        if (readbackFramebufferId == 0U) {
            if (error) {
                *error = QStringLiteral("The CPU publish bridge readback framebuffer is not initialized.");
            }
            return false;
        }

        gl.glBindFramebuffer(GL_FRAMEBUFFER, readbackFramebufferId);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sourceTextureId, 0);
        const GLenum status = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (error) {
                *error = QStringLiteral("The CPU publish bridge readback framebuffer is incomplete: 0x%1")
                             .arg(unsigned(status), 0, 16);
            }
            return false;
        }

        readbackBytes.resize(sourceSize.width() * sourceSize.height() * 4);
        gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl.glReadPixels(0,
                        0,
                        sourceSize.width(),
                        sourceSize.height(),
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        readbackBytes.data());
        const GLenum readbackError = gl.glGetError();
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (readbackError != GL_NO_ERROR) {
            if (error) {
                *error = QStringLiteral("glReadPixels failed in the CPU publish bridge: %1").arg(glErrorHex(readbackError));
            }
            return false;
        }

        return true;
    }

    bool convertToBgra(const QSize &sourceSize, QString *error)
    {
        Q_UNUSED(error);

        uploadBytes.resize(readbackBytes.size());
        const uchar *rgba = reinterpret_cast<const uchar *>(readbackBytes.constData());
        uchar *bgra = reinterpret_cast<uchar *>(uploadBytes.data());
        const int rowStride = sourceSize.width() * 4;
        for (int y = 0; y < sourceSize.height(); ++y) {
            const uchar *sourceRow = rgba + (sourceSize.height() - 1 - y) * rowStride;
            uchar *targetRow = bgra + y * rowStride;
            for (int x = 0; x < rowStride; x += 4) {
                targetRow[x + 0] = sourceRow[x + 2];
                targetRow[x + 1] = sourceRow[x + 1];
                targetRow[x + 2] = sourceRow[x + 0];
                targetRow[x + 3] = sourceRow[x + 3];
            }
        }
        return true;
    }

    bool uploadPixels(int slotIndex, const QSize &sourceSize, QString *error)
    {
        SlotResources &slot = slotResources[slotIndex];
        if (!slot.texture || !slot.keyedMutex) {
            if (error) {
                *error = QStringLiteral("The CPU publish bridge destination slot is not ready.");
            }
            return false;
        }

        for (;;) {
            const HRESULT acquireHr = slot.keyedMutex->AcquireSync(0, 1);
            if (SUCCEEDED(acquireHr)) {
                break;
            }
            if (isAcquireTimeout(acquireHr)) {
                continue;
            }
            if (error) {
                *error = QStringLiteral("AcquireSync(slot %1, key 0) failed: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(acquireHr));
            }
            return false;
        }

        D3D11_BOX box = {};
        box.left = 0;
        box.top = 0;
        box.front = 0;
        box.right = UINT(sourceSize.width());
        box.bottom = UINT(sourceSize.height());
        box.back = 1;
        runtime->d3d11DeviceContext()->UpdateSubresource(slot.texture.Get(),
                                                         0,
                                                         &box,
                                                         uploadBytes.constData(),
                                                         rowPitchForWidth(sourceSize.width()),
                                                         0);
        runtime->d3d11DeviceContext()->Flush();

        const HRESULT releaseHr = slot.keyedMutex->ReleaseSync(1);
        if (FAILED(releaseHr)) {
            if (error) {
                *error = QStringLiteral("ReleaseSync(slot %1, key 1) failed: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(releaseHr));
            }
            return false;
        }

        return true;
    }
};

D3D11CpuPublishBridge::D3D11CpuPublishBridge()
    : m_impl(std::make_unique<Impl>())
{
}

D3D11CpuPublishBridge::~D3D11CpuPublishBridge() = default;

bool D3D11CpuPublishBridge::initialize(AngleStandaloneRuntime *runtime,
                                       D3D11NativeSlotPool *slotPool,
                                       QString *error)
{
    return m_impl->initialize(runtime, slotPool, error);
}

bool D3D11CpuPublishBridge::publishToSlot(GLuint sourceTextureId,
                                          const QSize &sourceSize,
                                          int slotIndex,
                                          QString *error)
{
    return m_impl->publishToSlot(sourceTextureId, sourceSize, slotIndex, error);
}

void D3D11CpuPublishBridge::releaseGlResources()
{
    m_impl->releaseGlResources();
}
