#include "qt_angle_display_presenter.h"

#include "framework/backend/win_angle_d3d11/qt_angle_egl_tools.h"

#include <QtANGLE/EGL/eglext.h>

#include <array>
#include <dxgi.h>

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
}

QtAngleDisplayPresenter::QtAngleDisplayPresenter() = default;

QtAngleDisplayPresenter::~QtAngleDisplayPresenter()
{
    shutdown();
}

void QtAngleDisplayPresenter::setSlotPool(const std::shared_ptr<ISharedFrameSlotPool> &slotPool)
{
    m_slotPool = slotPool;
    m_importedSlots.resize(slotPool ? slotPool->slotCount() : 0);
}

QString QtAngleDisplayPresenter::lastRuntimeLog() const
{
    return m_runtimeLog;
}

bool QtAngleDisplayPresenter::initialize(IPresentationTarget &target, QString *error)
{
    auto *glTarget = dynamic_cast<IGlPresentationTarget *>(&target);
    if (glTarget == nullptr) {
        if (error) {
            *error = QStringLiteral("QtAngleDisplayPresenter requires a GL-capable presentation target.");
        }
        return false;
    }

    QString runtimeLog;
    const bool ok = initialize(glTarget, error, &runtimeLog);
    m_runtimeLog = runtimeLog;
    return ok;
}

bool QtAngleDisplayPresenter::enqueue(const PublicationTicket &ticket, QString *error)
{
    Q_UNUSED(error);

    PublishedFrame frame;
    frame.slotIndex = ticket.transportMetadata.value(QStringLiteral("slotIndex"), -1).toInt();
    frame.sharedHandle = ticket.transportMetadata.value(QStringLiteral("sharedHandle")).toULongLong();
    frame.size = ticket.transportMetadata.value(QStringLiteral("size")).toSize().isValid()
        ? ticket.transportMetadata.value(QStringLiteral("size")).toSize()
        : ticket.artifact.logicalSize;
    frame.generation = ticket.transportMetadata.value(QStringLiteral("generation")).toULongLong();
    frame.frameIndex = ticket.transportMetadata.value(QStringLiteral("frameIndex")).toULongLong();
    if (frame.slotIndex < 0) {
        if (error) {
            *error = QStringLiteral("Publication ticket is missing a valid slot index.");
        }
        return false;
    }

    consume(frame);
    if (m_target != nullptr) {
        m_target->requestPresent();
    }
    return true;
}

bool QtAngleDisplayPresenter::present(PresentationFeedback *feedback, QString *error)
{
    return paint(feedback, error);
}

void QtAngleDisplayPresenter::shutdown()
{
    if (!m_gl) {
        return;
    }

    for (int i = 0; i < m_importedSlots.size(); ++i) {
        destroyImportedSlot(i);
    }
    destroyDisplayTarget();
    m_initialized = false;
    m_gl = nullptr;
    m_eglApi = nullptr;
    m_eglDisplay = EGL_NO_DISPLAY;
    m_eglConfig = nullptr;
}

bool QtAngleDisplayPresenter::initialize(IGlPresentationTarget *target,
                                         QString *error,
                                         QString *runtimeLog)
{
    if (target == nullptr) {
        if (error) {
            *error = QStringLiteral("QtAngleDisplayPresenter requires a valid GL presentation target.");
        }
        return false;
    }

    QOpenGLContext *context = target->glContext();
    QOpenGLFunctions *functions = target->glFunctions();
    if (context == nullptr || functions == nullptr) {
        if (error) {
            *error = QStringLiteral("QtAngleDisplayPresenter requires a valid GL context and function table.");
        }
        return false;
    }

    m_target = target;
    m_gl = functions;

    if (!createProgram(error)) {
        return false;
    }

    QStringList probeLog;
    m_ownedEglApi = std::make_unique<QtAngleEglTools::ResolvedEglApi>(QtAngleEglTools::resolveEglApi(context, &probeLog));
    m_eglApi = m_ownedEglApi.get();
    if (!m_eglApi->supportsD3DTextureImport()) {
        if (error) {
            *error = QStringLiteral("D3D11 import presenter cannot resolve Qt EGL import functions.");
        }
        if (runtimeLog) {
            *runtimeLog = probeLog.join(QStringLiteral("\n"));
        }
        return false;
    }

    m_eglDisplay = QtAngleEglTools::queryDisplay(context, *m_eglApi, &probeLog);
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        if (error) {
            *error = QStringLiteral("D3D11 import presenter failed to resolve the Qt-owned EGLDisplay.");
        }
        if (runtimeLog) {
            *runtimeLog = probeLog.join(QStringLiteral("\n"));
        }
        return false;
    }

    m_eglConfig = QtAngleEglTools::queryConfig(context, &probeLog);
    const QtAngleEglTools::RendererIdentity identity =
        QtAngleEglTools::queryRendererIdentity(context, m_eglDisplay, *m_eglApi, &probeLog);
    if (runtimeLog) {
        *runtimeLog = QStringLiteral(
            "D3D11 import presenter is ready. UI runtime identity:\n"
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
                               probeLog.join(QStringLiteral("\n")));
    }

    m_initialized = true;
    onOutputSizeChanged(m_target->targetSize());
    return true;
}

void QtAngleDisplayPresenter::onOutputSizeChanged(const QSize &size)
{
    Q_UNUSED(size);
}

void QtAngleDisplayPresenter::consume(const PublishedFrame &frame)
{
    m_pendingFrame = frame;
    if (m_slotPool) {
        PublishedFrame slotInfo;
        if (m_slotPool->querySlot(frame.slotIndex, &slotInfo)) {
            m_pendingFrame.sharedHandle = slotInfo.sharedHandle;
        }
    }
    m_hasPendingFrame = true;
}

bool QtAngleDisplayPresenter::paint(PresentationFeedback *feedback, QString *error)
{
    if (feedback != nullptr) {
        feedback->releasedPublicationCapacity = false;
    }

    if (!m_gl || m_target == nullptr) {
        if (error) {
            *error = QStringLiteral("QtAngleDisplayPresenter is missing a valid GL presentation target or GL functions.");
        }
        return false;
    }

    const QSize viewportSize = m_target->targetSize();
    m_gl->glViewport(0, 0, viewportSize.width(), viewportSize.height());
    m_gl->glClear(GL_COLOR_BUFFER_BIT);

    if (m_hasPendingFrame && m_slotPool) {
        PublishedFrame pendingFrame;
        if (m_slotPool->consumePendingFrame(m_pendingFrame.slotIndex, &pendingFrame)) {
            const bool copied = copyFrameToDisplayTexture(pendingFrame, error);
            if (m_slotPool) {
                m_slotPool->releasePendingSlot(pendingFrame.slotIndex);
            }
            if (feedback != nullptr) {
                feedback->releasedPublicationCapacity = true;
            }
            if (copied) {
                m_displayFrame = pendingFrame;
                m_hasDisplayFrame = true;
            }
        }
        m_hasPendingFrame = false;
    }

    m_gl->glViewport(0, 0, viewportSize.width(), viewportSize.height());
    if (!m_hasDisplayFrame) {
        return true;
    }

    m_program.bind();
    m_gl->glActiveTexture(GL_TEXTURE0);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_displayTextureId);
    m_gl->glUniform1i(m_samplerLocation, 0);
    const std::array<GLfloat, 8> vertices = aspectFitVertices(m_displayFrame.size, viewportSize);
    m_gl->glVertexAttribPointer(m_positionLocation, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
    m_gl->glEnableVertexAttribArray(m_positionLocation);
    m_gl->glVertexAttribPointer(m_texCoordLocation, 2, GL_FLOAT, GL_FALSE, 0, kDisplayTexCoords);
    m_gl->glEnableVertexAttribArray(m_texCoordLocation);
    m_gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_gl->glDisableVertexAttribArray(m_positionLocation);
    m_gl->glDisableVertexAttribArray(m_texCoordLocation);
    m_gl->glBindTexture(GL_TEXTURE_2D, 0);
    m_program.release();
    return true;
}

bool QtAngleDisplayPresenter::createProgram(QString *error)
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

bool QtAngleDisplayPresenter::ensureDisplayTarget(const QSize &size, QString *error)
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

    m_gl->glGenTextures(1, &m_displayTextureId);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_displayTextureId);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_gl->glTexImage2D(GL_TEXTURE_2D,
                       0,
                       GL_RGBA,
                       size.width(),
                       size.height(),
                       0,
                       GL_RGBA,
                       GL_UNSIGNED_BYTE,
                       nullptr);
    m_gl->glBindTexture(GL_TEXTURE_2D, 0);

    m_gl->glGenFramebuffers(1, &m_displayFramebufferId);
    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_displayFramebufferId);
    m_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_displayTextureId, 0);
    const GLenum status = m_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

bool QtAngleDisplayPresenter::copyFrameToDisplayTexture(const PublishedFrame &frame, QString *error)
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
    m_gl->glBindTexture(GL_TEXTURE_2D, slot.textureId);
    if (m_eglApi->bindTexImage(m_eglDisplay, slot.surface, EGL_BACK_BUFFER) != EGL_TRUE) {
        if (error) {
            *error = QStringLiteral("eglBindTexImage(slot %1) failed with EGL error %2")
                         .arg(frame.slotIndex)
                         .arg(QtAngleEglTools::eglErrorToString(m_eglApi->getError()));
        }
    } else {
        slot.boundForRead = true;
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_displayFramebufferId);
        m_gl->glViewport(0, 0, frame.size.width(), frame.size.height());
        m_program.bind();
        m_gl->glActiveTexture(GL_TEXTURE0);
        m_gl->glBindTexture(GL_TEXTURE_2D, slot.textureId);
        m_gl->glUniform1i(m_samplerLocation, 0);
        m_gl->glVertexAttribPointer(m_positionLocation, 2, GL_FLOAT, GL_FALSE, 0, kVertices);
        m_gl->glEnableVertexAttribArray(m_positionLocation);
        m_gl->glVertexAttribPointer(m_texCoordLocation, 2, GL_FLOAT, GL_FALSE, 0, kImportedTexCoords);
        m_gl->glEnableVertexAttribArray(m_texCoordLocation);
        m_gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_gl->glDisableVertexAttribArray(m_positionLocation);
        m_gl->glDisableVertexAttribArray(m_texCoordLocation);
        m_gl->glBindTexture(GL_TEXTURE_2D, 0);
        m_program.release();
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_gl->glFlush();
        const GLenum glError = m_gl->glGetError();
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
    m_gl->glBindTexture(GL_TEXTURE_2D, 0);
    const HRESULT releaseHr = slot.keyedMutex->ReleaseSync(0);
    if (FAILED(releaseHr) && error && ok) {
        *error = QStringLiteral("ReleaseSync(slot %1, key 0) failed: 0x%2")
                     .arg(frame.slotIndex)
                     .arg(static_cast<unsigned int>(releaseHr), 0, 16);
        ok = false;
    }
    return ok;
}

bool QtAngleDisplayPresenter::ensureImportedSlot(int slotIndex, QString *error)
{
    if (!m_slotPool || !m_eglApi || slotIndex < 0 || slotIndex >= m_importedSlots.size()) {
        if (error) {
            *error = QStringLiteral("D3D11 import slot prerequisites are incomplete.");
        }
        return false;
    }

    PublishedFrame slotFrame;
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

    m_gl->glGenTextures(1, &slot.textureId);
    m_gl->glBindTexture(GL_TEXTURE_2D, slot.textureId);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_gl->glBindTexture(GL_TEXTURE_2D, 0);

    slot.generation = slotFrame.generation;
    slot.sharedHandle = slotFrame.sharedHandle;
    slot.size = slotFrame.size;
    return true;
}

void QtAngleDisplayPresenter::destroyImportedSlot(int slotIndex)
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

void QtAngleDisplayPresenter::destroyDisplayTarget()
{
    if (!m_gl) {
        return;
    }
    if (m_displayFramebufferId != 0U) {
        m_gl->glDeleteFramebuffers(1, &m_displayFramebufferId);
        m_displayFramebufferId = 0U;
    }
    if (m_displayTextureId != 0U) {
        m_gl->glDeleteTextures(1, &m_displayTextureId);
        m_displayTextureId = 0U;
    }
    m_displayTextureSize = QSize();
}
