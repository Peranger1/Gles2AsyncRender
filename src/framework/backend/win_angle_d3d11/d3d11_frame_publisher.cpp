#include "d3d11_frame_publisher.h"

#include "angle_standalone_runtime.h"
#include "framework/core/shared_frame_slot_pool.h"
#include "gles2_proc_table.h"
#include "gles2_shader_utils.h"
#include "runtime_diagnostics.h"

#include <QByteArray>
#include <QVector>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
enum class PublishMode
{
    Auto,
    ForceCpu
};

constexpr GLenum kBackBuffer = EGL_BACK_BUFFER;

constexpr GLfloat kVertices[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
    -1.0f,  1.0f,
     1.0f,  1.0f
};

constexpr GLfloat kTexCoords[] = {
    0.0f, 1.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 0.0f
};

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

QString publishModeToString(PublishMode mode)
{
    switch (mode) {
    case PublishMode::ForceCpu:
        return QStringLiteral("force_cpu");
    case PublishMode::Auto:
    default:
        return QStringLiteral("auto");
    }
}

PublishMode parsePublishMode(const QByteArray &value, QString *warning)
{
    const QByteArray normalized = value.trimmed().toLower();
    if (normalized.isEmpty() || normalized == "auto") {
        return PublishMode::Auto;
    }
    if (normalized == "force_cpu" || normalized == "cpu") {
        return PublishMode::ForceCpu;
    }

    if (warning) {
        *warning = QStringLiteral("Unknown GLES2ASYNC_PUBLISH_MODE value '%1'. Falling back to auto.")
                       .arg(QString::fromLocal8Bit(value));
    }
    return PublishMode::Auto;
}

void logPublishMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[D3D11FramePublisher]", message);
}

void logPublishWarning(const QString &message)
{
    RuntimeDiagnostics::logWarning("[D3D11FramePublisher]", message);
}

void logPublishDiag(const QString &message)
{
    RuntimeDiagnostics::logDiag("[D3D11FramePublisher]", message);
}
}

struct D3D11FramePublisher::Impl final
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

    AngleStandaloneRuntime *runtime = nullptr;
    ISharedFrameSlotPool *slotPool = nullptr;
    QVector<SlotResources> slotResources;
    QVector<ImportedSlot> importedSlots;
    GLuint publishFramebufferId = 0;
    GLuint readbackFramebufferId = 0;
    GLuint program = 0;
    GLint positionLocation = -1;
    GLint texCoordLocation = -1;
    GLint samplerLocation = -1;
    PublishMode publishMode = PublishMode::Auto;
    bool gpuPublishEnabled = false;
    bool gpuPathLogged = false;
    bool cpuFallbackLogged = false;
    QByteArray readbackBytes;
    QByteArray uploadBytes;

    bool initialize(AngleStandaloneRuntime *standaloneRuntime,
                    ISharedFrameSlotPool *slotPoolPtr,
                    QString *error)
    {
        runtime = standaloneRuntime;
        slotPool = slotPoolPtr;

        if (runtime == nullptr || slotPool == nullptr) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge requires both a runtime and a slot pool.");
            }
            return false;
        }

        QString modeWarning;
        publishMode = parsePublishMode(qgetenv("GLES2ASYNC_PUBLISH_MODE"), &modeWarning);
        if (!modeWarning.isEmpty()) {
            logPublishWarning(modeWarning);
        }

        const Gles2ProcTable &gl = runtime->procTable();
        if (!gl.isValid()) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge requires a valid standalone GLES2 proc table.");
            }
            return false;
        }

        gl.glGenFramebuffers(1, &publishFramebufferId);
        gl.glGenFramebuffers(1, &readbackFramebufferId);
        if (publishFramebufferId == 0U || readbackFramebufferId == 0U) {
            if (error) {
                *error = QStringLiteral("Failed to create the standalone publish bridge framebuffers.");
            }
            return false;
        }

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

        if (!Gles2ShaderUtils::buildProgram(gl, kVertexShader, kFragmentShader, &program, error)) {
            return false;
        }

        positionLocation = gl.glGetAttribLocation(program, "aPosition");
        texCoordLocation = gl.glGetAttribLocation(program, "aTexCoord");
        samplerLocation = gl.glGetUniformLocation(program, "uTexture");
        if (positionLocation < 0 || texCoordLocation < 0 || samplerLocation < 0) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge shader program is missing required attributes or uniforms.");
            }
            return false;
        }

        slotResources.resize(slotPool->slotCount());
        importedSlots.resize(slotPool->slotCount());
        const bool angleImportEntryPoints = runtime->procTable().supportsAngleD3DTextureImport();
        const bool hasDisplay = runtime->eglDisplay() != EGL_NO_DISPLAY;
        const bool hasConfig = runtime->eglConfig() != nullptr;
        QString initReason = QStringLiteral("standalone ANGLE GPU publish is available.");
        if (publishMode == PublishMode::ForceCpu) {
            initReason = QStringLiteral("publish mode is forced to CPU fallback by GLES2ASYNC_PUBLISH_MODE.");
        } else if (!angleImportEntryPoints) {
            initReason = QStringLiteral("required ANGLE D3D texture import entry points are unavailable.");
        } else if (!hasDisplay) {
            initReason = QStringLiteral("standalone runtime did not provide a valid EGLDisplay.");
        } else if (!hasConfig) {
            initReason = QStringLiteral("standalone runtime did not provide a valid EGLConfig.");
        }

        gpuPublishEnabled = publishMode != PublishMode::ForceCpu
            && angleImportEntryPoints
            && hasDisplay
            && hasConfig;

        logPublishMessage(QStringLiteral("Standalone publish bridge ready. requestedMode=%1 initialPath=%2 angleImportEntryPoints=%3 eglDisplay=%4 eglConfig=%5 reason=%6")
                              .arg(publishModeToString(publishMode))
                              .arg(gpuPublishEnabled ? QStringLiteral("gpu") : QStringLiteral("cpu"))
                              .arg(angleImportEntryPoints)
                              .arg(quintptr(runtime->eglDisplay()), 0, 16)
                              .arg(quintptr(runtime->eglConfig()), 0, 16)
                              .arg(initReason));
        logPublishMessage(QStringLiteral("Standalone publish bridge mode detail: %1").arg(initReason));
        return true;
    }

    bool publishToSlot(GLuint sourceTextureId,
                       const QSize &sourceSize,
                       int slotIndex,
                       QString *error)
    {
        if (runtime == nullptr || slotPool == nullptr) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge is not initialized.");
            }
            return false;
        }
        if (sourceTextureId == 0U) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge requires a valid source texture.");
            }
            return false;
        }

        const QSize safeSize = sanitizedSize(sourceSize);
        if (!ensureSlotResources(slotIndex, safeSize, error)) {
            return false;
        }

        if (gpuPublishEnabled) {
            QString gpuError;
            if (ensureImportedSlot(slotIndex, safeSize, &gpuError)
                && renderIntoImportedSlot(slotIndex, sourceTextureId, safeSize, &gpuError)) {
                if (!gpuPathLogged) {
                    gpuPathLogged = true;
                    logPublishMessage(QStringLiteral("Publish path selected: GPU publish."));
                }
                return true;
            }

            const QString fallbackMessage = QStringLiteral("GPU publish fallback to CPU for slot=%1 size=%2x%3 reason=%4")
                                                .arg(slotIndex)
                                                .arg(safeSize.width())
                                                .arg(safeSize.height())
                                                .arg(gpuError);
            if (!cpuFallbackLogged) {
                cpuFallbackLogged = true;
                logPublishMessage(fallbackMessage);
            } else {
                logPublishDiag(QStringLiteral("[diag] %1").arg(fallbackMessage));
            }
        }

        return publishViaCpuFallback(slotIndex, sourceTextureId, safeSize, error);
    }

    void releaseGlResources()
    {
        if (runtime == nullptr) {
            return;
        }

        const Gles2ProcTable &gl = runtime->procTable();
        for (int i = 0; i < importedSlots.size(); ++i) {
            destroyImportedSlot(i);
        }

        if (publishFramebufferId != 0U) {
            gl.glDeleteFramebuffers(1, &publishFramebufferId);
            publishFramebufferId = 0U;
        }
        if (readbackFramebufferId != 0U) {
            gl.glDeleteFramebuffers(1, &readbackFramebufferId);
            readbackFramebufferId = 0U;
        }

        Gles2ShaderUtils::deleteProgram(gl, &program);
        positionLocation = -1;
        texCoordLocation = -1;
        samplerLocation = -1;
    }

    bool ensureSlotResources(int slotIndex, const QSize &size, QString *error)
    {
        if (slotIndex < 0 || slotIndex >= slotResources.size()) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge slot index is out of range.");
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
        destroyImportedSlot(slotIndex);
        return true;
    }

    bool ensureImportedSlot(int slotIndex, const QSize &size, QString *error)
    {
        if (!gpuPublishEnabled) {
            if (error) {
                *error = QStringLiteral("ANGLE D3D texture import is unavailable on the standalone runtime.");
            }
            return false;
        }
        if (slotIndex < 0 || slotIndex >= importedSlots.size()) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge import slot index is out of range.");
            }
            return false;
        }

        PublishedFrame frame;
        if (!slotPool->querySlot(slotIndex, &frame)) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge import slot %1 is unavailable.").arg(slotIndex);
            }
            return false;
        }

        ImportedSlot &slot = importedSlots[slotIndex];
        if (slot.surface != EGL_NO_SURFACE
            && slot.sharedHandle == frame.sharedHandle
            && slot.generation == frame.generation
            && slot.size == size) {
            return true;
        }

        destroyImportedSlot(slotIndex);

        const EGLint surfaceAttributes[] = {
            EGL_WIDTH, size.width(),
            EGL_HEIGHT, size.height(),
            EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
            EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
            EGL_NONE
        };

        const Gles2ProcTable &gl = runtime->procTable();
        slot.surface = gl.eglCreatePbufferFromClientBuffer(runtime->eglDisplay(),
                                                           EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
                                                           reinterpret_cast<EGLClientBuffer>(frame.sharedHandle),
                                                           runtime->eglConfig(),
                                                           surfaceAttributes);
        if (slot.surface == EGL_NO_SURFACE) {
            if (error) {
                *error = QStringLiteral("eglCreatePbufferFromClientBuffer(slot %1) failed with EGL error %2")
                             .arg(slotIndex)
                             .arg(glErrorHex(gl.eglGetError()));
            }
            return false;
        }

        void *keyedMutexPtr = nullptr;
        if (gl.eglQuerySurfacePointerANGLE(runtime->eglDisplay(), slot.surface, EGL_DXGI_KEYED_MUTEX_ANGLE, &keyedMutexPtr) != EGL_TRUE
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

        gl.glGenTextures(1, &slot.textureId);
        gl.glBindTexture(GL_TEXTURE_2D, slot.textureId);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.glBindTexture(GL_TEXTURE_2D, 0);

        slot.sharedHandle = frame.sharedHandle;
        slot.generation = frame.generation;
        slot.size = size;
        return true;
    }

    void destroyImportedSlot(int slotIndex)
    {
        if (runtime == nullptr || slotIndex < 0 || slotIndex >= importedSlots.size()) {
            return;
        }

        ImportedSlot &slot = importedSlots[slotIndex];
        const Gles2ProcTable &gl = runtime->procTable();
        if (slot.textureId != 0U) {
            gl.glDeleteTextures(1, &slot.textureId);
            slot.textureId = 0U;
        }
        slot.keyedMutex.Reset();
        if (slot.surface != EGL_NO_SURFACE && gl.eglDestroySurface != nullptr) {
            gl.eglDestroySurface(runtime->eglDisplay(), slot.surface);
            slot.surface = EGL_NO_SURFACE;
        }
        slot.sharedHandle = 0;
        slot.generation = 0;
        slot.size = {};
    }

    bool renderIntoImportedSlot(int slotIndex,
                                GLuint sourceTextureId,
                                const QSize &sourceSize,
                                QString *error)
    {
        ImportedSlot &slot = importedSlots[slotIndex];
        if (!slot.keyedMutex || slot.surface == EGL_NO_SURFACE || slot.textureId == 0U) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge imported slot is not ready.");
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

        const Gles2ProcTable &gl = runtime->procTable();
        gl.glBindTexture(GL_TEXTURE_2D, slot.textureId);
        if (gl.eglBindTexImage(runtime->eglDisplay(), slot.surface, kBackBuffer) != EGL_TRUE) {
            gl.glBindTexture(GL_TEXTURE_2D, 0);
            slot.keyedMutex->ReleaseSync(0);
            if (error) {
                *error = QStringLiteral("eglBindTexImage(slot %1) failed with EGL error %2")
                             .arg(slotIndex)
                             .arg(glErrorHex(gl.eglGetError()));
            }
            return false;
        }

        gl.glBindFramebuffer(GL_FRAMEBUFFER, publishFramebufferId);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.textureId, 0);
        const GLenum status = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            gl.eglReleaseTexImage(runtime->eglDisplay(), slot.surface, kBackBuffer);
            gl.glBindTexture(GL_TEXTURE_2D, 0);
            slot.keyedMutex->ReleaseSync(0);
            if (error) {
                *error = QStringLiteral("The standalone publish bridge imported framebuffer is incomplete: 0x%1")
                             .arg(unsigned(status), 0, 16);
            }
            return false;
        }

        gl.glViewport(0, 0, sourceSize.width(), sourceSize.height());
        gl.glUseProgram(program);
        gl.glActiveTexture(GL_TEXTURE0);
        gl.glBindTexture(GL_TEXTURE_2D, sourceTextureId);
        gl.glUniform1i(samplerLocation, 0);
        gl.glVertexAttribPointer(GLuint(positionLocation), 2, GL_FLOAT, GL_FALSE, 0, kVertices);
        gl.glEnableVertexAttribArray(GLuint(positionLocation));
        gl.glVertexAttribPointer(GLuint(texCoordLocation), 2, GL_FLOAT, GL_FALSE, 0, kTexCoords);
        gl.glEnableVertexAttribArray(GLuint(texCoordLocation));
        gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gl.glDisableVertexAttribArray(GLuint(positionLocation));
        gl.glDisableVertexAttribArray(GLuint(texCoordLocation));
        gl.glBindTexture(GL_TEXTURE_2D, 0);
        gl.glUseProgram(0);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.glFlush();

        const GLenum glError = gl.glGetError();
        const EGLBoolean releaseImageOk = gl.eglReleaseTexImage(runtime->eglDisplay(), slot.surface, kBackBuffer);
        gl.glBindTexture(GL_TEXTURE_2D, 0);
        const HRESULT releaseHr = slot.keyedMutex->ReleaseSync(1);
        if (glError != GL_NO_ERROR) {
            if (error) {
                *error = QStringLiteral("Standalone GPU publish draw failed with GL error %1").arg(glErrorHex(glError));
            }
            return false;
        }
        if (releaseImageOk != EGL_TRUE) {
            if (error) {
                *error = QStringLiteral("eglReleaseTexImage(slot %1) failed with EGL error %2")
                             .arg(slotIndex)
                             .arg(glErrorHex(gl.eglGetError()));
            }
            return false;
        }
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
        if (!cpuFallbackLogged) {
            cpuFallbackLogged = true;
            logPublishMessage(QStringLiteral("Publish path selected: CPU fallback."));
        }

        if (!readPixels(sourceTextureId, sourceSize, error)
            || !convertToBgra(sourceSize)
            || !uploadPixels(slotIndex, sourceSize, error)) {
            return false;
        }
        return true;
    }

    bool readPixels(GLuint sourceTextureId, const QSize &sourceSize, QString *error)
    {
        const Gles2ProcTable &gl = runtime->procTable();
        if (readbackFramebufferId == 0U) {
            if (error) {
                *error = QStringLiteral("The standalone publish bridge readback framebuffer is not initialized.");
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
                *error = QStringLiteral("The standalone publish bridge readback framebuffer is incomplete: 0x%1")
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
                *error = QStringLiteral("glReadPixels failed in the standalone publish bridge: %1").arg(glErrorHex(readbackError));
            }
            return false;
        }

        return true;
    }

    bool convertToBgra(const QSize &sourceSize)
    {
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
                *error = QStringLiteral("The standalone publish bridge destination slot is not ready.");
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

        logPublishDiag(QStringLiteral("[diag] CPU fallback publish completed slot=%1 size=%2x%3")
                           .arg(slotIndex)
                           .arg(sourceSize.width())
                           .arg(sourceSize.height()));
        return true;
    }
};

D3D11FramePublisher::D3D11FramePublisher()
    : m_impl(std::make_unique<Impl>())
{
}

D3D11FramePublisher::~D3D11FramePublisher() = default;

bool D3D11FramePublisher::initialize(IRenderRuntime *runtime,
                                     ISharedFrameSlotPool *slotPool,
                                     QString *error)
{
    auto *standaloneRuntime = dynamic_cast<AngleStandaloneRuntime *>(runtime);
    if (standaloneRuntime == nullptr) {
        if (error) {
            *error = QStringLiteral("D3D11FramePublisher requires an AngleStandaloneRuntime.");
        }
        return false;
    }

    return m_impl->initialize(standaloneRuntime, slotPool, error);
}

bool D3D11FramePublisher::publishToSlot(GLuint sourceTextureId,
                                        const QSize &sourceSize,
                                        int slotIndex,
                                        QString *error)
{
    return m_impl->publishToSlot(sourceTextureId, sourceSize, slotIndex, error);
}

void D3D11FramePublisher::releaseGlResources()
{
    m_impl->releaseGlResources();
}
