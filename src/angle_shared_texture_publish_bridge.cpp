#include "angle_shared_texture_publish_bridge.h"

#include "d3d11_native_slot_pool.h"
#include "qt_angle_egl_tools.h"

#include <QByteArray>
#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QThread>
#include <QVector>

#include <QtANGLE/EGL/eglext.h>

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

void logPublishBridgeMessage(const QString &message)
{
    if (!message.isEmpty()) {
        qInfo().noquote() << "[PublishBridge]" << message;
    }
}

QString glErrorHex(GLenum error)
{
    return QStringLiteral("0x%1").arg(unsigned(error), 0, 16);
}

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

bool isAcquireTimeout(HRESULT hr)
{
    return hr == WAIT_TIMEOUT || hr == DXGI_ERROR_WAIT_TIMEOUT;
}

UINT alignRowPitch(int width)
{
    return UINT(qMax(1, width) * 4);
}

bool createD3D11Device(ComPtr<ID3D11Device> *device,
                       ComPtr<ID3D11DeviceContext> *context,
                       QString *error)
{
    if (device == nullptr || context == nullptr) {
        if (error) {
            *error = QStringLiteral("The publish bridge D3D11 prerequisites are incomplete.");
        }
        return false;
    }

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        kLevels,
        ARRAYSIZE(kLevels),
        D3D11_SDK_VERSION,
        device->ReleaseAndGetAddressOf(),
        &featureLevel,
        context->ReleaseAndGetAddressOf());

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            kLevels,
            ARRAYSIZE(kLevels),
            D3D11_SDK_VERSION,
            device->ReleaseAndGetAddressOf(),
            &featureLevel,
            context->ReleaseAndGetAddressOf());
    }

    if (FAILED(hr) || !(*device) || !(*context)) {
        if (error) {
            *error = QStringLiteral("The publish bridge D3D11 device creation failed: %1").arg(hresultToString(hr));
        }
        return false;
    }

    Q_UNUSED(featureLevel);
    return true;
}
} // namespace

struct AngleSharedTexturePublishBridge::Impl final
{
    struct SlotResources final
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> keyedMutex;
        quintptr sharedHandle = 0;
        QSize size;
        quint64 generation = 0;
    };

    struct ImportedSlot final
    {
        quintptr sharedHandle = 0;
        quint64 generation = 0;
        QSize size;
        EGLSurface surface = EGL_NO_SURFACE;
        GLuint textureId = 0;
        ComPtr<IDXGIKeyedMutex> keyedMutex;
    };

    QOpenGLContext *context = nullptr;
    D3D11NativeSlotPool *slotPool = nullptr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
    QVector<SlotResources> slotResources;
    QVector<ImportedSlot> importedSlots;

    QtAngleEglTools::ResolvedEglApi eglApi;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig eglConfig = nullptr;
    bool canGpuPublish = false;

    std::unique_ptr<QOpenGLShaderProgram> program;
    int positionLocation = -1;
    int texCoordLocation = -1;
    int samplerLocation = -1;
    GLuint framebufferId = 0;
    GLuint readbackFramebufferId = 0;
    QByteArray readbackBytes;

    bool initialize(QOpenGLContext *glContext,
                    D3D11NativeSlotPool *slotPoolPtr,
                    QString *error)
    {
        context = glContext;
        slotPool = slotPoolPtr;

        if (context == nullptr || slotPool == nullptr) {
            if (error) {
                *error = QStringLiteral("The publish bridge requires both a GL context and a slot pool.");
            }
            return false;
        }
        if (QOpenGLContext::currentContext() != context) {
            if (error) {
                *error = QStringLiteral("The publish bridge must initialize with its GL context current.");
            }
            return false;
        }
        if (!createD3D11Device(&device, &deviceContext, error)) {
            return false;
        }

        if (!createProgram(error)) {
            return false;
        }

        QOpenGLFunctions *functions = context->functions();
        if (functions == nullptr) {
            if (error) {
                *error = QStringLiteral("QOpenGLFunctions are unavailable for the publish bridge context.");
            }
            return false;
        }

        functions->glGenFramebuffers(1, &framebufferId);
        functions->glGenFramebuffers(1, &readbackFramebufferId);

        QStringList probeLog;
        eglApi = QtAngleEglTools::resolveEglApi(context, &probeLog);
        if (eglApi.supportsD3DTextureImport()) {
            eglDisplay = QtAngleEglTools::queryDisplay(context, eglApi, &probeLog);
            eglConfig = QtAngleEglTools::queryConfig(context, &probeLog);
            canGpuPublish = eglDisplay != EGL_NO_DISPLAY && eglConfig != nullptr;
        }

        Q_UNUSED(probeLog);
        canGpuPublish = false;

        slotResources.resize(slotPool->slotCount());
        importedSlots.resize(slotPool->slotCount());
        return true;
    }

    void releaseGlResources()
    {
        if (context == nullptr || QOpenGLContext::currentContext() != context) {
            return;
        }

        QOpenGLFunctions *functions = context->functions();
        if (functions == nullptr) {
            return;
        }

        for (ImportedSlot &slot : importedSlots) {
            if (slot.textureId != 0U) {
                functions->glDeleteTextures(1, &slot.textureId);
                slot.textureId = 0U;
            }
            slot.keyedMutex.Reset();
            if (slot.surface != EGL_NO_SURFACE && eglApi.destroySurface) {
                eglApi.destroySurface(eglDisplay, slot.surface);
                slot.surface = EGL_NO_SURFACE;
            }
            slot.sharedHandle = 0;
            slot.generation = 0;
            slot.size = QSize();
        }

        if (framebufferId != 0U) {
            functions->glDeleteFramebuffers(1, &framebufferId);
            framebufferId = 0U;
        }
        if (readbackFramebufferId != 0U) {
            functions->glDeleteFramebuffers(1, &readbackFramebufferId);
            readbackFramebufferId = 0U;
        }
        program.reset();
    }

    bool publishToSlot(GLuint sourceTextureId,
                       const QSize &sourceSize,
                       int slotIndex,
                       QString *error)
    {
        if (context == nullptr || slotPool == nullptr) {
            if (error) {
                *error = QStringLiteral("The publish bridge is not initialized.");
            }
            return false;
        }
        if (QOpenGLContext::currentContext() != context) {
            if (error) {
                *error = QStringLiteral("The publish bridge requires its GL context to be current.");
            }
            return false;
        }
        if (sourceTextureId == 0U) {
            if (error) {
                *error = QStringLiteral("The publish bridge requires a valid source texture id.");
            }
            return false;
        }

        const QSize safeSize = sanitizedSize(sourceSize);
        if (!ensureSlotResources(slotIndex, safeSize, error)) {
            return false;
        }

        QString gpuError;
        if (canGpuPublish && ensureImportedSlot(slotIndex, &gpuError) && renderIntoImportedSlot(slotIndex, sourceTextureId, safeSize, &gpuError)) {
            return true;
        }

        return publishViaCpuFallback(slotIndex, sourceTextureId, safeSize, error ? error : &gpuError);
    }

    bool createProgram(QString *error)
    {
        auto shaderProgram = std::make_unique<QOpenGLShaderProgram>();

        static const char *kVertexShader = R"(
attribute highp vec2 aPosition;
attribute mediump vec2 aTexCoord;
varying mediump vec2 vTexCoord;

void main()
{
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

        static const char *kFragmentShader = R"(
precision mediump float;

varying mediump vec2 vTexCoord;
uniform sampler2D uTexture;

void main()
{
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)";

        if (!shaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)) {
            if (error) {
                *error = shaderProgram->log();
            }
            return false;
        }
        if (!shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)) {
            if (error) {
                *error = shaderProgram->log();
            }
            return false;
        }
        if (!shaderProgram->link()) {
            if (error) {
                *error = shaderProgram->log();
            }
            return false;
        }

        positionLocation = shaderProgram->attributeLocation("aPosition");
        texCoordLocation = shaderProgram->attributeLocation("aTexCoord");
        samplerLocation = shaderProgram->uniformLocation("uTexture");
        if (positionLocation < 0 || texCoordLocation < 0 || samplerLocation < 0) {
            if (error) {
                *error = QStringLiteral("The publish bridge shader program is missing attributes or uniforms.");
            }
            return false;
        }

        program = std::move(shaderProgram);
        return true;
    }

    bool ensureSlotResources(int slotIndex, const QSize &size, QString *error)
    {
        if (slotIndex < 0 || slotIndex >= slotResources.size()) {
            if (error) {
                *error = QStringLiteral("The publish bridge render slot is out of range.");
            }
            return false;
        }

        SlotResources &slot = slotResources[slotIndex];
        const QSize safeSize = sanitizedSize(size);
        if (slot.texture && slot.size == safeSize && slot.sharedHandle != 0) {
            return true;
        }

        slot.texture.Reset();
        slot.keyedMutex.Reset();
        slot.sharedHandle = 0;
        slot.size = QSize();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = UINT(safeSize.width());
        desc.Height = UINT(safeSize.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &slot.texture);
        if (FAILED(hr) || !slot.texture) {
            if (error) {
                *error = QStringLiteral("The publish bridge could not create slot %1: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(hr));
            }
            return false;
        }

        hr = slot.texture.As(&slot.keyedMutex);
        if (FAILED(hr) || !slot.keyedMutex) {
            if (error) {
                *error = QStringLiteral("The publish bridge could not query IDXGIKeyedMutex for slot %1: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(hr));
            }
            return false;
        }

        ComPtr<IDXGIResource> resource;
        hr = slot.texture.As(&resource);
        if (FAILED(hr) || !resource) {
            if (error) {
                *error = QStringLiteral("The publish bridge could not query IDXGIResource for slot %1: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(hr));
            }
            return false;
        }

        HANDLE sharedHandle = nullptr;
        hr = resource->GetSharedHandle(&sharedHandle);
        if (FAILED(hr) || sharedHandle == nullptr) {
            if (error) {
                *error = QStringLiteral("The publish bridge could not get the shared handle for slot %1: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(hr));
            }
            return false;
        }

        slot.sharedHandle = quintptr(sharedHandle);
        slot.size = safeSize;
        ++slot.generation;
        slotPool->updateSlot(slotIndex, slot.sharedHandle, slot.size, slot.generation);
        destroyImportedSlot(slotIndex);
        return true;
    }

    bool ensureImportedSlot(int slotIndex, QString *error)
    {
        if (!canGpuPublish || slotIndex < 0 || slotIndex >= importedSlots.size()) {
            return false;
        }

        D3D11NativeFrame frame;
        if (!slotPool->querySlot(slotIndex, &frame)) {
            if (error) {
                *error = QStringLiteral("The publish bridge import slot %1 is unavailable.").arg(slotIndex);
            }
            return false;
        }

        ImportedSlot &slot = importedSlots[slotIndex];
        if (slot.surface != EGL_NO_SURFACE
            && slot.sharedHandle == frame.sharedHandle
            && slot.generation == frame.generation) {
            return true;
        }

        destroyImportedSlot(slotIndex);

        const EGLint surfaceAttributes[] = {
            EGL_WIDTH, frame.size.width(),
            EGL_HEIGHT, frame.size.height(),
            EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
            EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
            EGL_NONE
        };

        slot.surface = eglApi.createPbufferFromClientBuffer(
            eglDisplay,
            EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
            reinterpret_cast<EGLClientBuffer>(frame.sharedHandle),
            eglConfig,
            surfaceAttributes);
        if (slot.surface == EGL_NO_SURFACE) {
            if (error) {
                *error = QStringLiteral("eglCreatePbufferFromClientBuffer(slot %1) failed with EGL error %2")
                             .arg(slotIndex)
                             .arg(QtAngleEglTools::eglErrorToString(eglApi.getError()));
            }
            return false;
        }

        void *keyedMutexPtr = nullptr;
        if (eglApi.querySurfacePointerANGLE(eglDisplay, slot.surface, EGL_DXGI_KEYED_MUTEX_ANGLE, &keyedMutexPtr) != EGL_TRUE
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

        QOpenGLFunctions *functions = context->functions();
        functions->glGenTextures(1, &slot.textureId);
        functions->glBindTexture(GL_TEXTURE_2D, slot.textureId);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        functions->glBindTexture(GL_TEXTURE_2D, 0);

        slot.sharedHandle = frame.sharedHandle;
        slot.generation = frame.generation;
        slot.size = frame.size;
        return true;
    }

    void destroyImportedSlot(int slotIndex)
    {
        if (slotIndex < 0 || slotIndex >= importedSlots.size()) {
            return;
        }

        ImportedSlot &slot = importedSlots[slotIndex];
        QOpenGLFunctions *functions = context ? context->functions() : nullptr;
        if (functions && slot.textureId != 0U) {
            functions->glDeleteTextures(1, &slot.textureId);
            slot.textureId = 0U;
        }

        slot.keyedMutex.Reset();
        if (slot.surface != EGL_NO_SURFACE && eglApi.destroySurface) {
            eglApi.destroySurface(eglDisplay, slot.surface);
            slot.surface = EGL_NO_SURFACE;
        }

        slot.sharedHandle = 0;
        slot.generation = 0;
        slot.size = QSize();
    }

    bool renderIntoImportedSlot(int slotIndex,
                                GLuint sourceTextureId,
                                const QSize &sourceSize,
                                QString *error)
    {
        ImportedSlot &slot = importedSlots[slotIndex];
        if (!slot.keyedMutex || slot.surface == EGL_NO_SURFACE || slot.textureId == 0U) {
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

        QOpenGLFunctions *functions = context->functions();
        static const GLfloat kVertices[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f
        };
        static const GLfloat kTexCoords[] = {
            0.0f, 1.0f,
            1.0f, 1.0f,
            0.0f, 0.0f,
            1.0f, 0.0f
        };

        functions->glBindTexture(GL_TEXTURE_2D, slot.textureId);
        if (eglApi.bindTexImage(eglDisplay, slot.surface, EGL_BACK_BUFFER) != EGL_TRUE) {
            functions->glBindTexture(GL_TEXTURE_2D, 0);
            slot.keyedMutex->ReleaseSync(0);
            if (error) {
                *error = QStringLiteral("eglBindTexImage(slot %1) failed with EGL error %2")
                             .arg(slotIndex)
                             .arg(QtAngleEglTools::eglErrorToString(eglApi.getError()));
            }
            return false;
        }

        functions->glBindFramebuffer(GL_FRAMEBUFFER, framebufferId);
        functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.textureId, 0);
        const GLenum status = functions->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            functions->glBindFramebuffer(GL_FRAMEBUFFER, 0);
            eglApi.releaseTexImage(eglDisplay, slot.surface, EGL_BACK_BUFFER);
            functions->glBindTexture(GL_TEXTURE_2D, 0);
            slot.keyedMutex->ReleaseSync(0);
            if (error) {
                *error = QStringLiteral("The publish bridge imported framebuffer is incomplete: 0x%1")
                             .arg(unsigned(status), 0, 16);
            }
            return false;
        }

        functions->glViewport(0, 0, sourceSize.width(), sourceSize.height());
        functions->glDisable(GL_BLEND);
        program->bind();
        functions->glActiveTexture(GL_TEXTURE0);
        functions->glBindTexture(GL_TEXTURE_2D, sourceTextureId);
        program->setUniformValue(samplerLocation, 0);
        functions->glVertexAttribPointer(positionLocation, 2, GL_FLOAT, GL_FALSE, 0, kVertices);
        functions->glEnableVertexAttribArray(positionLocation);
        functions->glVertexAttribPointer(texCoordLocation, 2, GL_FLOAT, GL_FALSE, 0, kTexCoords);
        functions->glEnableVertexAttribArray(texCoordLocation);
        functions->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        functions->glDisableVertexAttribArray(positionLocation);
        functions->glDisableVertexAttribArray(texCoordLocation);
        functions->glBindTexture(GL_TEXTURE_2D, 0);
        program->release();
        functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        functions->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        functions->glFlush();

        eglApi.releaseTexImage(eglDisplay, slot.surface, EGL_BACK_BUFFER);
        functions->glBindTexture(GL_TEXTURE_2D, 0);
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

    bool publishViaCpuFallback(int slotIndex,
                               GLuint sourceTextureId,
                               const QSize &sourceSize,
                               QString *error)
    {
        logPublishBridgeMessage(QStringLiteral("[diag] publishViaCpuFallback begin slot=%1 sourceTex=%2 size=%3x%4 ctx=%5 thread=%6")
                                    .arg(slotIndex)
                                    .arg(sourceTextureId)
                                    .arg(sourceSize.width())
                                    .arg(sourceSize.height())
                                    .arg(reinterpret_cast<quintptr>(context), 0, 16)
                                    .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16));
        if (!readPixels(sourceTextureId, sourceSize, error)) {
            return false;
        }
        return uploadPixels(slotIndex, sourceSize, error);
    }

    bool readPixels(GLuint sourceTextureId, const QSize &sourceSize, QString *error)
    {
        QOpenGLFunctions *functions = context->functions();
        if (functions == nullptr || readbackFramebufferId == 0U) {
            if (error) {
                *error = QStringLiteral("The publish bridge readback path is not initialized.");
            }
            return false;
        }

        logPublishBridgeMessage(QStringLiteral("[diag] readPixels begin readbackFbo=%1 sourceTex=%2 size=%3x%4 currentCtx=%5")
                                    .arg(readbackFramebufferId)
                                    .arg(sourceTextureId)
                                    .arg(sourceSize.width())
                                    .arg(sourceSize.height())
                                    .arg(reinterpret_cast<quintptr>(QOpenGLContext::currentContext()), 0, 16));

        functions->glBindFramebuffer(GL_FRAMEBUFFER, readbackFramebufferId);
        logPublishBridgeMessage(QStringLiteral("[diag] readPixels after glBindFramebuffer glError=%1")
                                    .arg(glErrorHex(functions->glGetError())));
        functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sourceTextureId, 0);
        logPublishBridgeMessage(QStringLiteral("[diag] readPixels after glFramebufferTexture2D sourceTex=%1 glError=%2")
                                    .arg(sourceTextureId)
                                    .arg(glErrorHex(functions->glGetError())));
        const GLenum status = functions->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        logPublishBridgeMessage(QStringLiteral("[diag] readPixels framebufferStatus=0x%1 glError=%2")
                                    .arg(unsigned(status), 0, 16)
                                    .arg(glErrorHex(functions->glGetError())));
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            functions->glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (error) {
                *error = QStringLiteral("The publish bridge readback framebuffer is incomplete: 0x%1")
                             .arg(unsigned(status), 0, 16);
            }
            return false;
        }

        readbackBytes.resize(sourceSize.width() * sourceSize.height() * 4);
        logPublishBridgeMessage(QStringLiteral("[diag] readPixels before glReadPixels bytes=%1")
                                    .arg(readbackBytes.size()));
        functions->glReadPixels(0,
                                0,
                                sourceSize.width(),
                                sourceSize.height(),
                                GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                readbackBytes.data());
        logPublishBridgeMessage(QStringLiteral("[diag] readPixels after glReadPixels glError=%1")
                                    .arg(glErrorHex(functions->glGetError())));
        functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        functions->glBindFramebuffer(GL_FRAMEBUFFER, 0);

        const GLenum finalError = functions->glGetError();
        logPublishBridgeMessage(QStringLiteral("[diag] readPixels end finalGlError=%1")
                                    .arg(glErrorHex(finalError)));
        return finalError == GL_NO_ERROR;
    }

    bool uploadPixels(int slotIndex, const QSize &sourceSize, QString *error)
    {
        if (slotIndex < 0 || slotIndex >= slotResources.size()) {
            if (error) {
                *error = QStringLiteral("The publish bridge fallback slot is out of range.");
            }
            return false;
        }

        SlotResources &slot = slotResources[slotIndex];
        if (!slot.texture || !slot.keyedMutex) {
            if (error) {
                *error = QStringLiteral("The publish bridge fallback slot is not initialized.");
            }
            return false;
        }

        QByteArray bgraBytes(readbackBytes.size(), Qt::Uninitialized);
        const uchar *rgba = reinterpret_cast<const uchar *>(readbackBytes.constData());
        uchar *bgra = reinterpret_cast<uchar *>(bgraBytes.data());
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

        for (;;) {
            const HRESULT acquireHr = slot.keyedMutex->AcquireSync(0, 1);
            if (SUCCEEDED(acquireHr)) {
                break;
            }
            if (isAcquireTimeout(acquireHr)) {
                continue;
            }
            if (error) {
                *error = QStringLiteral("AcquireSync(slot %1, key 0) failed in fallback mode: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(acquireHr));
            }
            return false;
        }

        D3D11_BOX updateBox = {};
        updateBox.left = 0;
        updateBox.top = 0;
        updateBox.front = 0;
        updateBox.right = UINT(sourceSize.width());
        updateBox.bottom = UINT(sourceSize.height());
        updateBox.back = 1;

        deviceContext->UpdateSubresource(slot.texture.Get(),
                                         0,
                                         &updateBox,
                                         bgraBytes.constData(),
                                         alignRowPitch(sourceSize.width()),
                                         0);
        deviceContext->Flush();

        const HRESULT releaseHr = slot.keyedMutex->ReleaseSync(1);
        if (FAILED(releaseHr)) {
            if (error) {
                *error = QStringLiteral("ReleaseSync(slot %1, key 1) failed in fallback mode: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(releaseHr));
            }
            return false;
        }

        return true;
    }
};

AngleSharedTexturePublishBridge::AngleSharedTexturePublishBridge()
    : m_impl(std::make_unique<Impl>())
{
}

AngleSharedTexturePublishBridge::~AngleSharedTexturePublishBridge() = default;

bool AngleSharedTexturePublishBridge::initialize(QOpenGLContext *context,
                                                 D3D11NativeSlotPool *slotPool,
                                                 QString *error)
{
    return m_impl->initialize(context, slotPool, error);
}

bool AngleSharedTexturePublishBridge::publishToSlot(GLuint sourceTextureId,
                                                    const QSize &sourceSize,
                                                    int slotIndex,
                                                    QString *error)
{
    return m_impl->publishToSlot(sourceTextureId, sourceSize, slotIndex, error);
}

void AngleSharedTexturePublishBridge::releaseGlResources()
{
    m_impl->releaseGlResources();
}
