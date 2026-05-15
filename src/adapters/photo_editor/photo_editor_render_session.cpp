#include "photo_editor_render_session.h"

#include "photo_editor_gles2_backend.h"
#include "framework/backend/win_angle_d3d11/angle_standalone_runtime.h"

#include <mutex>

namespace
{
QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

std::once_flag g_photoEditorInitOnce;
bool g_photoEditorInitSucceeded = false;
QString g_photoEditorInitError;
AngleStandaloneRuntime *g_photoEditorRuntime = nullptr;
}

void PhotoEditorRenderSession::SessionState::reset()
{
    handle = nullptr;
    latestParameters = {};
    processingParameters = {};
    hasLatestParameters = false;
    parametersDirty = false;
    latestProgress = 0;
    processInFlight = false;
    renderReady = false;
}

bool PhotoEditorRenderSession::initializeLibraryOnce(AngleStandaloneRuntime *runtime, QString *error)
{
    if (runtime == nullptr) {
        if (error) {
            *error = QStringLiteral("Photo editor render session requires a valid standalone runtime.");
        }
        return false;
    }

    if (!runtime->procTable().isValid()) {
        if (error) {
            *error = QStringLiteral("Photo editor render session requires an initialized standalone proc table.");
        }
        return false;
    }

    if (g_photoEditorRuntime != nullptr && g_photoEditorRuntime != runtime) {
        if (error) {
            *error = QStringLiteral("photo_editor_init is already bound to a different standalone runtime.");
        }
        return false;
    }

    std::call_once(g_photoEditorInitOnce, [runtime]() {
        QString initError;
        g_photoEditorRuntime = runtime;
        g_photoEditorInitSucceeded = photo_editor_init(&PhotoEditorRenderSession::resolveGlProc, &initError);
        if (!g_photoEditorInitSucceeded) {
            g_photoEditorInitError = initError;
        }
    });

    if (!g_photoEditorInitSucceeded) {
        if (error) {
            *error = g_photoEditorInitError.isEmpty()
                ? QStringLiteral("photo_editor_init failed.")
                : g_photoEditorInitError;
        }
        return false;
    }

    return true;
}

void *PhotoEditorRenderSession::resolveGlProc(const char *name)
{
    if (g_photoEditorRuntime == nullptr || name == nullptr) {
        return nullptr;
    }

    return g_photoEditorRuntime->resolveProc(name);
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

    const bool ok = initializeLibraryOnce(m_runtime, error);
    m_runtime->leave();
    return ok;
}

bool PhotoEditorRenderSession::submitRequest(const std::shared_ptr<PhotoEditorRenderPayload> &payload,
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

    if (!payload || payload->sourceImage.isNull()) {
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

    const QSize safeOutputSize = sanitizedSize(payload->outputSize.isValid()
        ? payload->outputSize
        : payload->sourceImage.size());
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

bool PhotoEditorRenderSession::renderReadyTexture(GLuint *textureId, QSize *size, QString *error)
{
    if (textureId == nullptr || size == nullptr) {
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

    GLuint renderedTextureId = 0U;
    QSize textureSize;
    const bool ok = photo_editor_render(m_session.handle, &renderedTextureId, &textureSize, error);
    if (!ok) {
        return false;
    }

    *textureId = renderedTextureId;
    *size = textureSize;
    m_session.renderReady = false;
    m_session.latestProgress = 100;
    return true;
}

void PhotoEditorRenderSession::shutdown()
{
    destroySession();
    m_runtime = nullptr;
    m_sourceKey.clear();
    m_sourceImageCacheKey = 0;
    m_sourceImage = QImage();
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
