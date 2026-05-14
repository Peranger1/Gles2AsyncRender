#include "photo_editor_render_session.h"

#include "photo_editor_render_payload.h"
#include "photo_editor_gles2_simulator.h"
#include "framework/backend/win_angle_d3d11/angle_standalone_runtime.h"

namespace
{
QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}
}

bool PhotoEditorRenderSession::initialize(AngleStandaloneRuntime *runtime, QString *error)
{
    m_runtime = runtime;
    if (m_runtime == nullptr) {
        if (error) {
            *error = QStringLiteral("The photo editor render session runtime is invalid.");
        }
        return false;
    }
    if (!m_runtime->enter(error)) {
        return false;
    }

    const bool ok = m_libraryHost.initializeOnce(m_runtime, error);
    m_runtime->leave();
    return ok;
}

bool PhotoEditorRenderSession::submitRequest(const PhotoEditorRequest &request,
                                             QObject *callbackContext,
                                             PhotoEditorProgressCallback progressCallback,
                                             void *progressUserData,
                                             QString *error)
{
    if (m_runtime == nullptr) {
        if (error) {
            *error = QStringLiteral("The photo editor render session runtime is not initialized.");
        }
        return false;
    }
    if (callbackContext == nullptr || progressCallback == nullptr) {
        if (error) {
            *error = QStringLiteral("The photo editor render session requires a valid progress callback target.");
        }
        return false;
    }

    const PhotoEditorRenderPayload *payload =
        static_cast<const PhotoEditorRenderPayload *>(request.payload.get());
    if (payload == nullptr || payload->sourceImage.isNull()) {
        if (error) {
            *error = QStringLiteral("The photo editor render payload is missing a valid source image.");
        }
        return false;
    }
    if (m_session.processInFlight) {
        if (error) {
            *error = QStringLiteral("The photo editor render session is still processing.");
        }
        return false;
    }

    const QSize safeOutputSize = sanitizedSize(request.outputSize);
    if (!ensureSessionForPayload(payload->sourceKey,
                                 payload->sourceImageCacheKey,
                                 payload->sourceImage,
                                 safeOutputSize,
                                 error)) {
        return false;
    }

    if (!m_runtime->enter(error)) {
        return false;
    }

    bool ok = photo_editor_set_output_size(m_session.handle, safeOutputSize, error);
    if (ok) {
        ok = photo_editor_set_opcode(m_session.handle, payload->parameters, error);
    }
    if (ok) {
        ok = photo_editor_process(m_session.handle,
                                  callbackContext,
                                  progressCallback,
                                  progressUserData,
                                  error);
    }
    m_runtime->leave();

    if (!ok) {
        return false;
    }

    m_requestSequence = request.sequence;
    m_session.latestParameters = payload->parameters;
    m_session.processingParameters = payload->parameters;
    m_session.hasLatestParameters = true;
    m_session.parametersDirty = false;
    m_session.processInFlight = true;
    m_session.renderReady = false;
    m_session.latestProgress = 0;
    return true;
}

void PhotoEditorRenderSession::handleProgressEvent(int progress, bool isEnd)
{
    m_session.latestProgress = progress;
    if (!isEnd) {
        return;
    }

    m_session.processInFlight = false;
    m_session.renderReady = true;
}

bool PhotoEditorRenderSession::isProcessInFlight() const noexcept
{
    return m_session.processInFlight;
}

bool PhotoEditorRenderSession::hasRenderReady() const noexcept
{
    return m_session.renderReady;
}

int PhotoEditorRenderSession::latestProgress() const noexcept
{
    return m_session.latestProgress;
}

quint64 PhotoEditorRenderSession::requestSequence() const noexcept
{
    return m_requestSequence;
}

bool PhotoEditorRenderSession::renderReadyTexture(PhotoEditorRenderedTexture *output, QString *error)
{
    if (output == nullptr) {
        if (error) {
            *error = QStringLiteral("The photo editor render output target is invalid.");
        }
        return false;
    }
    if (m_runtime == nullptr || m_session.handle == nullptr || !m_session.renderReady) {
        if (error) {
            *error = QStringLiteral("No completed photo editor result is ready to render.");
        }
        return false;
    }

    GLuint textureId = 0U;
    QSize textureSize;
    const bool ok = photo_editor_render(m_session.handle, &textureId, &textureSize, error);
    if (!ok) {
        return false;
    }

    output->textureId = textureId;
    output->size = textureSize;
    m_session.renderReady = false;
    m_session.latestProgress = 100;
    return true;
}

void PhotoEditorRenderSession::discardRenderReady()
{
    m_session.renderReady = false;
}

void PhotoEditorRenderSession::shutdown()
{
    destroySession();
    m_runtime = nullptr;
    m_sourceKey.clear();
    m_sourceImageCacheKey = 0;
    m_sourceImage = QImage();
    m_requestSequence = 0;
}

bool PhotoEditorRenderSession::ensureSessionForPayload(const QString &sourceKey,
                                                       quint64 sourceImageCacheKey,
                                                       const QImage &sourceImage,
                                                       const QSize &outputSize,
                                                       QString *error)
{
    if (m_session.handle == nullptr
        || m_sourceKey != sourceKey
        || m_sourceImageCacheKey != sourceImageCacheKey) {
        m_sourceKey = sourceKey;
        m_sourceImageCacheKey = sourceImageCacheKey;
        m_sourceImage = sourceImage;
        return rebuildSession(sourceImage, outputSize, error);
    }

    if (!m_runtime->enter(error)) {
        return false;
    }
    const bool ok = photo_editor_set_output_size(m_session.handle, outputSize, error);
    m_runtime->leave();
    return ok;
}

bool PhotoEditorRenderSession::rebuildSession(const QImage &sourceImage,
                                              const QSize &outputSize,
                                              QString *error)
{
    destroySession();
    if (sourceImage.isNull()) {
        if (error) {
            *error = QStringLiteral("The photo editor source image is invalid.");
        }
        return false;
    }
    if (m_runtime == nullptr) {
        if (error) {
            *error = QStringLiteral("The photo editor render session runtime is not initialized.");
        }
        return false;
    }
    if (!m_runtime->enter(error)) {
        return false;
    }

    m_session.handle = photo_editor_create(sourceImage, error);
    bool ok = m_session.handle != nullptr;
    if (ok) {
        ok = photo_editor_set_output_size(m_session.handle, outputSize, error);
    }
    m_runtime->leave();
    if (!ok) {
        destroySession();
        return false;
    }

    m_session.latestParameters = {};
    m_session.processingParameters = {};
    m_session.hasLatestParameters = false;
    m_session.parametersDirty = false;
    m_session.latestProgress = 0;
    m_session.processInFlight = false;
    m_session.renderReady = false;
    return true;
}

void PhotoEditorRenderSession::destroySession()
{
    if (m_session.handle == nullptr) {
        m_session.reset();
        return;
    }

    const bool madeCurrent = m_runtime && m_runtime->enter(nullptr);
    photo_editor_destroy(m_session.handle);
    if (madeCurrent) {
        m_runtime->leave();
    }
    m_session.reset();
}
