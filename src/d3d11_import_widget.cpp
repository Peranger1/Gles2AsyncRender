#include "d3d11_import_widget.h"

#include "angle_threading.h"
#include "qt_angle_egl_tools.h"
#include "runtime_diagnostics.h"

#include <QtANGLE/EGL/eglext.h>

#include <array>
#include <dxgi.h>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr GLuint kBackBuffer = EGL_BACK_BUFFER;

constexpr GLfloat kVertices[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
    -1.0f,  1.0f,
     1.0f,  1.0f
};

constexpr GLfloat kImportedTexCoords[] = {
    0.0f, 1.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 0.0f
};

constexpr GLfloat kDisplayTexCoords[] = {
    0.0f, 0.0f,
    1.0f, 0.0f,
    0.0f, 1.0f,
    1.0f, 1.0f
};

std::array<GLfloat, 8> aspectFitVertices(const QSize &contentSize, const QSize &viewportSize)
{
    if (!contentSize.isValid() || !viewportSize.isValid()) {
        return { -1.0f, -1.0f,
                  1.0f, -1.0f,
                 -1.0f,  1.0f,
                  1.0f,  1.0f };
    }

    const qreal contentAspect = qreal(contentSize.width()) / qMax(1, contentSize.height());
    const qreal viewportAspect = qreal(viewportSize.width()) / qMax(1, viewportSize.height());

    GLfloat halfWidth = 1.0f;
    GLfloat halfHeight = 1.0f;
    if (contentAspect > viewportAspect) {
        halfHeight = GLfloat(viewportAspect / contentAspect);
    } else {
        halfWidth = GLfloat(contentAspect / viewportAspect);
    }

    return { -halfWidth, -halfHeight,
              halfWidth, -halfHeight,
             -halfWidth,  halfHeight,
              halfWidth,  halfHeight };
}

void logImportWidgetMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[D3D11ImportWidget]", message);
}

void logImportWidgetDiag(const QString &message)
{
    RuntimeDiagnostics::logDiag("[D3D11ImportWidget]", message);
}
}

D3D11ImportWidget::D3D11ImportWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    connect(this, &QOpenGLWidget::frameSwapped, this, &D3D11ImportWidget::notifyDisplayReadyForWorker, Qt::QueuedConnection);
}

D3D11ImportWidget::~D3D11ImportWidget()
{
    m_shuttingDown = true;
    disconnect(this, &QOpenGLWidget::frameSwapped, this, &D3D11ImportWidget::notifyDisplayReadyForWorker);
    logImportWidgetDiag(QStringLiteral("Destructor begin hasDisplayFrame=%1 hasPendingFrame=%2")
                            .arg(m_hasDisplayFrame)
                            .arg(m_hasPendingFrame));
    if (context()) {
        makeCurrent();
        for (int i = 0; i < m_importedSlots.size(); ++i) {
            destroyImportedSlot(i);
        }
        destroyDisplayTarget();
        doneCurrent();
    }
    logImportWidgetDiag(QStringLiteral("Destructor end"));
}

void D3D11ImportWidget::setSlotPool(const std::shared_ptr<D3D11NativeSlotPool> &slotPool)
{
    m_slotPool = slotPool;
    m_importedSlots.resize(slotPool ? slotPool->slotCount() : 0);
}

QSize D3D11ImportWidget::outputPixelSize() const
{
    const qreal dpr = devicePixelRatioF();
    return QSize(qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
}

void D3D11ImportWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.05f, 0.06f, 0.08f, 1.0f);

    const AngleThreadingInfo angleInfo = ensureAngleD3D11MultithreadProtection();
    logImportWidgetMessage(angleInfo.message);

    QString error;
    if (!createProgram(&error)) {
        logImportWidgetMessage(QStringLiteral("D3D11 import widget shader initialization failed: %1").arg(error));
        return;
    }

    QStringList probeLog;
    m_ownedEglApi = std::make_unique<QtAngleEglTools::ResolvedEglApi>(QtAngleEglTools::resolveEglApi(context(), &probeLog));
    m_eglApi = m_ownedEglApi.get();
    if (!m_eglApi->supportsD3DTextureImport()) {
        logImportWidgetMessage(QStringLiteral("D3D11 import widget cannot resolve Qt EGL import functions.\n%1")
                                   .arg(probeLog.join(QStringLiteral("\n"))));
        return;
    }

    m_eglDisplay = QtAngleEglTools::queryDisplay(context(), *m_eglApi, &probeLog);
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        logImportWidgetMessage(QStringLiteral("D3D11 import widget failed to resolve the Qt-owned EGLDisplay.\n%1")
                                   .arg(probeLog.join(QStringLiteral("\n"))));
        return;
    }

    m_eglConfig = QtAngleEglTools::queryConfig(context(), &probeLog);
    const QtAngleEglTools::RendererIdentity identity =
        QtAngleEglTools::queryRendererIdentity(context(), m_eglDisplay, *m_eglApi, &probeLog);
    logImportWidgetMessage(QStringLiteral(
                               "D3D11 import widget is ready. UI runtime identity:\n"
                               "  EGL module=%1 (%2)\n"
                               "  EGLDisplay=%3\n"
                               "  EGLDevice=%4\n"
                               "  D3D11Device=%5\n"
                               "  AdapterLuid=%6\n"
                               "  EGL vendor=%7 version=%8\n"
                               "  GL vendor=%9 renderer=%10 version=%11\n"
                               "%12")
                               .arg(QtAngleEglTools::pointerToString(identity.eglModule),
                                    identity.eglModulePath,
                                    QtAngleEglTools::pointerToString(identity.eglDisplay),
                                    QtAngleEglTools::pointerToString(reinterpret_cast<const void *>(identity.eglDevice)),
                                    QtAngleEglTools::pointerToString(identity.d3d11Device),
                                    identity.adapterLuid,
                                    identity.eglVendor,
                                    identity.eglVersion,
                                    identity.glVendor,
                                    identity.glRenderer,
                                    identity.glVersion,
                                    probeLog.join(QStringLiteral("\n"))));
    emit outputSizeChanged(outputPixelSize());
    emit glInitialized();
    m_workerReadyPending = true;
}

void D3D11ImportWidget::resizeGL(int, int)
{
    emit outputSizeChanged(outputPixelSize());
}

void D3D11ImportWidget::paintGL()
{
    const QSize viewportSize = outputPixelSize();
    glViewport(0, 0, viewportSize.width(), viewportSize.height());
    glClear(GL_COLOR_BUFFER_BIT);

    if (m_hasPendingFrame && m_slotPool) {
        D3D11NativeFrame pendingFrame;
        if (m_slotPool->consumePendingFrame(m_pendingFrame.slotIndex, &pendingFrame)) {
            QString error;
            const bool copied = copyFrameToDisplayTexture(pendingFrame, &error);
            if (m_slotPool) {
                m_slotPool->releasePendingSlot(pendingFrame.slotIndex);
            }
            m_workerReadyPending = true;
            emit slotAvailableForWorker();
            if (!copied) {
                logImportWidgetMessage(error);
            } else {
                m_displayFrame = pendingFrame;
                m_hasDisplayFrame = true;
            }
        }
        m_hasPendingFrame = false;
    }

    glViewport(0, 0, viewportSize.width(), viewportSize.height());

    if (!m_hasDisplayFrame) {
        return;
    }

    m_program.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_displayTextureId);
    glUniform1i(m_samplerLocation, 0);
    const std::array<GLfloat, 8> vertices = aspectFitVertices(m_displayFrame.size, viewportSize);
    glVertexAttribPointer(m_positionLocation, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
    glEnableVertexAttribArray(m_positionLocation);
    glVertexAttribPointer(m_texCoordLocation, 2, GL_FLOAT, GL_FALSE, 0, kDisplayTexCoords);
    glEnableVertexAttribArray(m_texCoordLocation);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(m_positionLocation);
    glDisableVertexAttribArray(m_texCoordLocation);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_program.release();
}

void D3D11ImportWidget::onFrameReady(int slotIndex, quint64 generation, QSize size, quint64 frameIndex)
{
    if (m_shuttingDown) {
        return;
    }

    m_pendingFrame.slotIndex = slotIndex;
    m_pendingFrame.generation = generation;
    m_pendingFrame.size = size;
    m_pendingFrame.frameIndex = frameIndex;
    if (m_slotPool) {
        D3D11NativeFrame slotInfo;
        if (m_slotPool->querySlot(slotIndex, &slotInfo)) {
            m_pendingFrame.sharedHandle = slotInfo.sharedHandle;
        }
    }
    m_hasPendingFrame = true;
    update();
}

void D3D11ImportWidget::notifyDisplayReadyForWorker()
{
    if (m_shuttingDown || !m_workerReadyPending) {
        return;
    }

    m_workerReadyPending = false;
    emit displayReadyForWorker();
}

bool D3D11ImportWidget::ensureDisplayTarget(const QSize &size, QString *error)
{
    if (!size.isValid()) {
        if (error) {
            *error = QStringLiteral("The display target size is invalid.");
        }
        return false;
    }

    if (m_displayTextureId != 0U && m_displayFramebufferId != 0U && m_displayTextureSize == size) {
        return true;
    }

    destroyDisplayTarget();

    glGenTextures(1, &m_displayTextureId);
    glBindTexture(GL_TEXTURE_2D, m_displayTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 size.width(),
                 size.height(),
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_displayFramebufferId);
    glBindFramebuffer(GL_FRAMEBUFFER, m_displayFramebufferId);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_displayTextureId, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error) {
            *error = QStringLiteral("The UI display framebuffer is incomplete: 0x%1")
                         .arg(unsigned(status), 0, 16);
        }
        destroyDisplayTarget();
        return false;
    }

    m_displayTextureSize = size;
    return true;
}

bool D3D11ImportWidget::copyFrameToDisplayTexture(const D3D11NativeFrame &frame, QString *error)
{
    if (!ensureImportedSlot(frame.slotIndex, error)) {
        return false;
    }
    if (!ensureDisplayTarget(frame.size, error)) {
        return false;
    }

    ImportedSlot &slot = m_importedSlots[frame.slotIndex];
    const HRESULT acquireHr = slot.keyedMutex->AcquireSync(1, 5);
    if (acquireHr != S_OK) {
        if (error) {
            *error = QStringLiteral("AcquireSync(slot %1, key 1) failed: 0x%2")
                         .arg(frame.slotIndex)
                         .arg(static_cast<unsigned int>(acquireHr), 0, 16);
        }
        return false;
    }

    bool ok = false;
    glBindTexture(GL_TEXTURE_2D, slot.textureId);
    if (m_eglApi->bindTexImage(m_eglDisplay, slot.surface, EGL_BACK_BUFFER) != EGL_TRUE) {
        if (error) {
            *error = QStringLiteral("eglBindTexImage(slot %1) failed with EGL error %2")
                         .arg(frame.slotIndex)
                         .arg(QtAngleEglTools::eglErrorToString(m_eglApi->getError()));
        }
    } else {
        slot.boundForRead = true;
        glBindFramebuffer(GL_FRAMEBUFFER, m_displayFramebufferId);
        glViewport(0, 0, frame.size.width(), frame.size.height());
        m_program.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, slot.textureId);
        glUniform1i(m_samplerLocation, 0);
        glVertexAttribPointer(m_positionLocation, 2, GL_FLOAT, GL_FALSE, 0, kVertices);
        glEnableVertexAttribArray(m_positionLocation);
        glVertexAttribPointer(m_texCoordLocation, 2, GL_FLOAT, GL_FALSE, 0, kImportedTexCoords);
        glEnableVertexAttribArray(m_texCoordLocation);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(m_positionLocation);
        glDisableVertexAttribArray(m_texCoordLocation);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_program.release();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFlush();
        const GLenum glError = glGetError();
        if (glError != GL_NO_ERROR) {
            if (error) {
                *error = QStringLiteral("UI copy-on-acquire draw failed with GL error 0x%1")
                             .arg(unsigned(glError), 0, 16);
            }
        } else {
            ok = true;
        }
    }

    if (slot.boundForRead) {
        if (m_eglApi->releaseTexImage(m_eglDisplay, slot.surface, EGL_BACK_BUFFER) != EGL_TRUE && error && ok) {
            *error = QStringLiteral("eglReleaseTexImage(slot %1) failed with EGL error %2")
                         .arg(frame.slotIndex)
                         .arg(QtAngleEglTools::eglErrorToString(m_eglApi->getError()));
            ok = false;
        }
        slot.boundForRead = false;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    const HRESULT releaseHr = slot.keyedMutex->ReleaseSync(0);
    if (FAILED(releaseHr) && error && ok) {
        *error = QStringLiteral("ReleaseSync(slot %1, key 0) failed: 0x%2")
                     .arg(frame.slotIndex)
                     .arg(static_cast<unsigned int>(releaseHr), 0, 16);
        ok = false;
    }
    return ok;
}

bool D3D11ImportWidget::createProgram(QString *error)
{
    static const char *vertexShader = R"(
        attribute highp vec2 aPosition;
        attribute mediump vec2 aTexCoord;
        varying mediump vec2 vTexCoord;

        void main()
        {
            vTexCoord = aTexCoord;
            gl_Position = vec4(aPosition, 0.0, 1.0);
        }
    )";

    static const char *fragmentShader = R"(
        varying mediump vec2 vTexCoord;
        uniform sampler2D uTexture;

        void main()
        {
            gl_FragColor = texture2D(uTexture, vTexCoord);
        }
    )";

    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)) {
        if (error) {
            *error = m_program.log();
        }
        return false;
    }

    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)) {
        if (error) {
            *error = m_program.log();
        }
        return false;
    }

    if (!m_program.link()) {
        if (error) {
            *error = m_program.log();
        }
        return false;
    }

    m_positionLocation = m_program.attributeLocation("aPosition");
    m_texCoordLocation = m_program.attributeLocation("aTexCoord");
    m_samplerLocation = m_program.uniformLocation("uTexture");
    return m_positionLocation >= 0 && m_texCoordLocation >= 0 && m_samplerLocation >= 0;
}

bool D3D11ImportWidget::ensureImportedSlot(int slotIndex, QString *error)
{
    if (!m_slotPool || !m_eglApi || slotIndex < 0 || slotIndex >= m_importedSlots.size()) {
        if (error) {
            *error = QStringLiteral("D3D11 import slot prerequisites are incomplete.");
        }
        return false;
    }

    D3D11NativeFrame slotFrame;
    if (!m_slotPool->querySlot(slotIndex, &slotFrame)) {
        if (error) {
            *error = QStringLiteral("D3D11 import slot %1 is unavailable.").arg(slotIndex);
        }
        return false;
    }

    ImportedSlot &slot = m_importedSlots[slotIndex];
    if (slot.surface != EGL_NO_SURFACE
        && slot.generation == slotFrame.generation
        && slot.sharedHandle == slotFrame.sharedHandle) {
        return true;
    }

    destroyImportedSlot(slotIndex);

    const EGLint surfaceAttributes[] = {
        EGL_WIDTH, slotFrame.size.width(),
        EGL_HEIGHT, slotFrame.size.height(),
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_NONE
    };

    slot.surface = m_eglApi->createPbufferFromClientBuffer(
        m_eglDisplay,
        EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
        reinterpret_cast<EGLClientBuffer>(slotFrame.sharedHandle),
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

    glGenTextures(1, &slot.textureId);
    glBindTexture(GL_TEXTURE_2D, slot.textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    slot.generation = slotFrame.generation;
    slot.sharedHandle = slotFrame.sharedHandle;
    slot.size = slotFrame.size;
    return true;
}

void D3D11ImportWidget::destroyImportedSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= m_importedSlots.size()) {
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
        glDeleteTextures(1, &slot.textureId);
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

void D3D11ImportWidget::destroyDisplayTarget()
{
    if (m_displayFramebufferId != 0U) {
        glDeleteFramebuffers(1, &m_displayFramebufferId);
        m_displayFramebufferId = 0U;
    }
    if (m_displayTextureId != 0U) {
        glDeleteTextures(1, &m_displayTextureId);
        m_displayTextureId = 0U;
    }
    m_displayTextureSize = QSize();
}
