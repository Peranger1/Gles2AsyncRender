#include "photo_editor_gles2_backend.h"

#include "framework/backend/win_angle_d3d11/gles2_proc_table.h"
#include "framework/backend/win_angle_d3d11/gles2_shader_utils.h"
#include "runtime_diagnostics.h"

#include <QByteArray>
#include <QThread>
#include <QTimer>
#include <QtMath>

#include <memory>

namespace
{
Gles2ProcTable g_procTable;
bool g_initialized = false;

struct PhotoEditorHandleState final
{
    QImage sourceImage;
    bool sourceImageDirty = true;

    ImageEffectParameters parameters;
    QSize outputSize;

    bool processActive = false;
    bool processCompleted = false;
    quint64 processGeneration = 0;
    bool destroyed = false;

    GLuint sourceTextureId = 0;
    GLuint outputTextureId = 0;
    GLuint framebufferId = 0;
    QSize outputTextureSize;

    GLuint program = 0;
    GLint positionLocation = -1;
    GLint texCoordLocation = -1;
    GLint sourceLocation = -1;
    GLint sourceSizeLocation = -1;
    GLint outputSizeLocation = -1;
    GLint brightnessLocation = -1;
    GLint contrastLocation = -1;
    GLint zoomLocation = -1;
    GLint panLocation = -1;
    GLint rotationLocation = -1;
    GLint flipLocation = -1;
    GLint heavyPassLocation = -1;
};

struct PhotoEditorHandle final
{
    std::shared_ptr<PhotoEditorHandleState> state;
};

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

void logSimulatorDiag(const QString &message)
{
    RuntimeDiagnostics::logDiag("[PhotoEditorSim]", message);
}

QString glErrorHex(GLenum error)
{
    return QStringLiteral("0x%1").arg(unsigned(error), 0, 16);
}

PhotoEditorHandle *toHandle(void *handle)
{
    return static_cast<PhotoEditorHandle *>(handle);
}

bool currentProcTable(QString *error)
{
    if (g_initialized && g_procTable.isValid()) {
        return true;
    }

    if (error) {
        *error = QStringLiteral("photo_editor_init must complete before using the GLES2 backend.");
    }
    return false;
}

bool ensureProgram(PhotoEditorHandleState *state, QString *error)
{
    if (state == nullptr) {
        if (error) {
            *error = QStringLiteral("The backend program state is missing.");
        }
        return false;
    }
    if (!currentProcTable(error)) {
        return false;
    }
    if (state->program != 0U) {
        return true;
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

uniform sampler2D uSource;
uniform mediump vec2 uSourceSize;
uniform mediump vec2 uOutputSize;
uniform mediump float uBrightness;
uniform mediump float uContrast;
uniform mediump float uZoom;
uniform mediump vec2 uPan;
uniform mediump float uRotationRadians;
uniform mediump vec2 uFlip;
uniform int uHeavyPassCount;

void main()
{
    mediump vec2 centered = vTexCoord - vec2(0.5, 0.5);
    mediump float sourceAspect = uSourceSize.x / max(uSourceSize.y, 1.0);
    mediump float outputAspect = uOutputSize.x / max(uOutputSize.y, 1.0);

    mediump vec2 fitHalfExtent = vec2(0.5, 0.5);
    if (sourceAspect > outputAspect) {
        fitHalfExtent.y *= outputAspect / sourceAspect;
    } else {
        fitHalfExtent.x *= sourceAspect / outputAspect;
    }

    mediump vec2 p = centered / max(fitHalfExtent * 2.0, vec2(1e-5, 1e-5));
    p -= uPan;

    mediump float s = sin(-uRotationRadians);
    mediump float c = cos(-uRotationRadians);
    p = vec2(c * p.x - s * p.y, s * p.x + c * p.y);

    mediump float safeZoom = max(uZoom, 0.05);
    p /= safeZoom;
    p *= uFlip;

    mediump vec2 imageUv = p + vec2(0.5, 0.5);
    mediump vec2 clampedUv = clamp(imageUv, vec2(0.0), vec2(1.0));
    mediump vec4 sampled = texture2D(uSource, clampedUv);

    mediump float inBounds = step(0.0, imageUv.x) * step(imageUv.x, 1.0)
                           * step(0.0, imageUv.y) * step(imageUv.y, 1.0);

    mediump vec2 grid = p * vec2(6.0, 6.0);
    mediump float checker = mod(floor(grid.x) + floor(grid.y), 2.0);
    mediump float rings = 0.5 + 0.5 * cos(18.0 * length(p));
    mediump vec3 backdrop = mix(vec3(0.05, 0.06, 0.08), vec3(0.16, 0.18, 0.22), checker * 0.35 + rings * 0.15);
    mediump vec3 accum = mix(backdrop, sampled.rgb, inBounds);

    for (int i = 0; i < 64; ++i) {
        if (i >= uHeavyPassCount) {
            break;
        }
        mediump float fi = float(i) + 1.0;
        mediump float wobble = sin(fi * 0.17 + p.x * (4.0 + fi * 0.02))
                             * cos(fi * 0.11 + p.y * (5.0 + fi * 0.03));
        accum += vec3(
            wobble * 0.0025,
            sin(wobble + fi * 0.09) * 0.0018,
            cos(wobble - fi * 0.05) * 0.0015);
        p += vec2(wobble * 0.0004, -wobble * 0.0003);
    }

    mediump vec3 color = (accum - 0.5) * uContrast + (0.5 + uBrightness);
    gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
)";

    if (!Gles2ShaderUtils::buildProgram(g_procTable, kVertexShader, kFragmentShader, &state->program, error)) {
        return false;
    }

    state->positionLocation = g_procTable.glGetAttribLocation(state->program, "aPosition");
    state->texCoordLocation = g_procTable.glGetAttribLocation(state->program, "aTexCoord");
    state->sourceLocation = g_procTable.glGetUniformLocation(state->program, "uSource");
    state->sourceSizeLocation = g_procTable.glGetUniformLocation(state->program, "uSourceSize");
    state->outputSizeLocation = g_procTable.glGetUniformLocation(state->program, "uOutputSize");
    state->brightnessLocation = g_procTable.glGetUniformLocation(state->program, "uBrightness");
    state->contrastLocation = g_procTable.glGetUniformLocation(state->program, "uContrast");
    state->zoomLocation = g_procTable.glGetUniformLocation(state->program, "uZoom");
    state->panLocation = g_procTable.glGetUniformLocation(state->program, "uPan");
    state->rotationLocation = g_procTable.glGetUniformLocation(state->program, "uRotationRadians");
    state->flipLocation = g_procTable.glGetUniformLocation(state->program, "uFlip");
    state->heavyPassLocation = g_procTable.glGetUniformLocation(state->program, "uHeavyPassCount");

    if (state->positionLocation < 0
        || state->texCoordLocation < 0
        || state->sourceLocation < 0
        || state->sourceSizeLocation < 0
        || state->outputSizeLocation < 0
        || state->brightnessLocation < 0
        || state->contrastLocation < 0
        || state->zoomLocation < 0
        || state->panLocation < 0
        || state->rotationLocation < 0
        || state->flipLocation < 0
        || state->heavyPassLocation < 0) {
        if (error) {
            *error = QStringLiteral("The backend shader program is missing required attributes or uniforms.");
        }
        Gles2ShaderUtils::deleteProgram(g_procTable, &state->program);
        return false;
    }

    return true;
}

bool ensureSourceTexture(PhotoEditorHandleState *state, QString *error)
{
    if (!currentProcTable(error) || state == nullptr) {
        return false;
    }

    if (state->sourceTextureId == 0U) {
        g_procTable.glGenTextures(1, &state->sourceTextureId);
    }
    if (!state->sourceImageDirty) {
        return true;
    }

    const QImage image = state->sourceImage.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) {
        if (error) {
            *error = QStringLiteral("The backend source image is invalid.");
        }
        return false;
    }

    g_procTable.glBindTexture(GL_TEXTURE_2D, state->sourceTextureId);
    g_procTable.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_procTable.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_procTable.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g_procTable.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_procTable.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g_procTable.glTexImage2D(GL_TEXTURE_2D,
                             0,
                             GL_RGBA,
                             image.width(),
                             image.height(),
                             0,
                             GL_RGBA,
                             GL_UNSIGNED_BYTE,
                             image.constBits());
    g_procTable.glBindTexture(GL_TEXTURE_2D, 0);
    state->sourceImageDirty = false;
    return true;
}

bool ensureOutputTarget(PhotoEditorHandleState *state, QString *error)
{
    if (!currentProcTable(error) || state == nullptr) {
        return false;
    }

    const QSize targetSize = sanitizedSize(state->outputSize.isValid() ? state->outputSize : state->sourceImage.size());
    if (state->outputTextureId == 0U) {
        g_procTable.glGenTextures(1, &state->outputTextureId);
    }
    if (state->framebufferId == 0U) {
        g_procTable.glGenFramebuffers(1, &state->framebufferId);
    }

    if (state->outputTextureSize != targetSize) {
        g_procTable.glBindTexture(GL_TEXTURE_2D, state->outputTextureId);
        g_procTable.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g_procTable.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_procTable.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g_procTable.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_procTable.glTexImage2D(GL_TEXTURE_2D,
                                 0,
                                 GL_RGBA,
                                 targetSize.width(),
                                 targetSize.height(),
                                 0,
                                 GL_RGBA,
                                 GL_UNSIGNED_BYTE,
                                 nullptr);
        g_procTable.glBindTexture(GL_TEXTURE_2D, 0);
        state->outputTextureSize = targetSize;
    }

    g_procTable.glBindFramebuffer(GL_FRAMEBUFFER, state->framebufferId);
    g_procTable.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, state->outputTextureId, 0);
    const GLenum status = g_procTable.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    g_procTable.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error) {
            *error = QStringLiteral("The backend framebuffer is incomplete: 0x%1")
                         .arg(unsigned(status), 0, 16);
        }
        return false;
    }

    return true;
}

void releaseGlResources(PhotoEditorHandleState *state)
{
    if (state == nullptr || !g_initialized || !g_procTable.isValid()) {
        return;
    }

    if (state->framebufferId != 0U) {
        g_procTable.glDeleteFramebuffers(1, &state->framebufferId);
        state->framebufferId = 0U;
    }
    if (state->outputTextureId != 0U) {
        g_procTable.glDeleteTextures(1, &state->outputTextureId);
        state->outputTextureId = 0U;
    }
    if (state->sourceTextureId != 0U) {
        g_procTable.glDeleteTextures(1, &state->sourceTextureId);
        state->sourceTextureId = 0U;
    }
    Gles2ShaderUtils::deleteProgram(g_procTable, &state->program);
    state->outputTextureSize = {};
}

void scheduleProgressStep(const std::shared_ptr<PhotoEditorHandleState> &state,
                          QObject *callbackContext,
                          PhotoEditorProgressCallback callback,
                          void *userData,
                          quint64 generation,
                          int delayMs,
                          int progress,
                          bool isEnd)
{
    QTimer::singleShot(delayMs, callbackContext, [state, callback, userData, generation, progress, isEnd]() {
        if (!state || state->destroyed || state->processGeneration != generation) {
            return;
        }

        if (isEnd) {
            state->processActive = false;
            state->processCompleted = true;
        }

        if (callback != nullptr) {
            callback(progress, isEnd, userData);
        }
    });
}
}

bool photo_editor_init(PhotoEditorResolveGlProc resolver, QString *error)
{
    if (resolver == nullptr) {
        if (error) {
            *error = QStringLiteral("photo_editor_init requires a GL proc resolver.");
        }
        return false;
    }

    Gles2ProcTable procTable;
    const auto resolve = [resolver](const char *name) { return resolver(name); };
    if (!procTable.load(resolve, error)) {
        return false;
    }
    if (procTable.eglGetCurrentContext() == EGL_NO_CONTEXT) {
        if (error) {
            *error = QStringLiteral("photo_editor_init requires a current standalone EGL context.");
        }
        return false;
    }
    if (procTable.glGetString(GL_VENDOR) == nullptr) {
        if (error) {
            *error = QStringLiteral("The GL proc resolver could not resolve glGetString.");
        }
        return false;
    }

    g_procTable = procTable;
    g_initialized = true;
    return true;
}

void *photo_editor_create(const QImage &sourceImage, QString *error)
{
    Q_UNUSED(error);

    if (sourceImage.isNull()) {
        if (error) {
            *error = QStringLiteral("photo_editor_create requires a valid source image.");
        }
        return nullptr;
    }

    auto handle = std::make_unique<PhotoEditorHandle>();
    handle->state = std::make_shared<PhotoEditorHandleState>();
    handle->state->sourceImage = sourceImage.convertToFormat(QImage::Format_RGBA8888);
    handle->state->outputSize = sanitizedSize(sourceImage.size());
    return handle.release();
}

void photo_editor_destroy(void *handle)
{
    std::unique_ptr<PhotoEditorHandle> ownedHandle(toHandle(handle));
    if (!ownedHandle || !ownedHandle->state) {
        return;
    }

    ownedHandle->state->destroyed = true;
    ++ownedHandle->state->processGeneration;
    ownedHandle->state->processActive = false;
    ownedHandle->state->processCompleted = false;
    releaseGlResources(ownedHandle->state.get());
}

bool photo_editor_set_output_size(void *handle, const QSize &outputSize, QString *error)
{
    Q_UNUSED(error);

    PhotoEditorHandle *photoHandle = toHandle(handle);
    if (photoHandle == nullptr || !photoHandle->state) {
        if (error) {
            *error = QStringLiteral("photo_editor_set_output_size requires a valid handle.");
        }
        return false;
    }

    photoHandle->state->outputSize = sanitizedSize(outputSize);
    return true;
}

bool photo_editor_set_opcode(void *handle, const ImageEffectParameters &parameters, QString *error)
{
    Q_UNUSED(error);

    PhotoEditorHandle *photoHandle = toHandle(handle);
    if (photoHandle == nullptr || !photoHandle->state) {
        if (error) {
            *error = QStringLiteral("photo_editor_set_opcode requires a valid handle.");
        }
        return false;
    }

    photoHandle->state->parameters = parameters;
    return true;
}

bool photo_editor_process(void *handle,
                          QObject *callbackContext,
                          PhotoEditorProgressCallback callback,
                          void *userData,
                          QString *error)
{
    PhotoEditorHandle *photoHandle = toHandle(handle);
    if (photoHandle == nullptr || !photoHandle->state) {
        if (error) {
            *error = QStringLiteral("photo_editor_process requires a valid handle.");
        }
        return false;
    }
    if (callbackContext == nullptr) {
        if (error) {
            *error = QStringLiteral("photo_editor_process requires a callback context.");
        }
        return false;
    }
    if (photoHandle->state->processActive) {
        if (error) {
            *error = QStringLiteral("photo_editor_process does not support re-entry while processing is active.");
        }
        return false;
    }

    photoHandle->state->processActive = true;
    photoHandle->state->processCompleted = false;
    const quint64 generation = ++photoHandle->state->processGeneration;
    const std::shared_ptr<PhotoEditorHandleState> state = photoHandle->state;

    scheduleProgressStep(state, callbackContext, callback, userData, generation, 10, 20, false);
    scheduleProgressStep(state, callbackContext, callback, userData, generation, 30, 55, false);
    scheduleProgressStep(state, callbackContext, callback, userData, generation, 55, 100, true);
    return true;
}

bool photo_editor_render(void *handle, GLuint *textureId, QSize *size, QString *error)
{
    PhotoEditorHandle *photoHandle = toHandle(handle);
    if (photoHandle == nullptr || !photoHandle->state) {
        if (error) {
            *error = QStringLiteral("photo_editor_render requires a valid handle.");
        }
        return false;
    }
    if (!photoHandle->state->processCompleted) {
        if (error) {
            *error = QStringLiteral("photo_editor_render requires a completed process result.");
        }
        return false;
    }
    if (textureId == nullptr || size == nullptr) {
        if (error) {
            *error = QStringLiteral("photo_editor_render requires valid output pointers.");
        }
        return false;
    }
    if (!currentProcTable(error)) {
        return false;
    }

    PhotoEditorHandleState *state = photoHandle->state.get();
    if (!ensureProgram(state, error) || !ensureSourceTexture(state, error) || !ensureOutputTarget(state, error)) {
        return false;
    }

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

    QThread::currentThread()->msleep(150);

    const QSize targetSize = state->outputTextureSize;
    const QImage sourceImage = state->sourceImage;
    const int heavyPassCount = qBound(0, state->parameters.heavyGpuPassCount, 64);

    logSimulatorDiag(QStringLiteral("[diag] render begin ctx=%1 display=%2 thread=%3 sourceTex=%4 outputTex=%5 fbo=%6 target=%7x%8 brightness=%9 contrast=%10 zoom=%11 heavyPass=%12")
                            .arg(quintptr(g_procTable.eglGetCurrentContext()), 0, 16)
                            .arg(quintptr(g_procTable.eglGetCurrentDisplay()), 0, 16)
                            .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16)
                            .arg(state->sourceTextureId)
                            .arg(state->outputTextureId)
                            .arg(state->framebufferId)
                            .arg(targetSize.width())
                            .arg(targetSize.height())
                            .arg(QString::number(state->parameters.brightness, 'f', 3))
                            .arg(QString::number(state->parameters.contrast, 'f', 3))
                            .arg(QString::number(state->parameters.zoom, 'f', 3))
                            .arg(heavyPassCount));

    g_procTable.glBindFramebuffer(GL_FRAMEBUFFER, state->framebufferId);
    g_procTable.glViewport(0, 0, targetSize.width(), targetSize.height());
    g_procTable.glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    g_procTable.glClear(GL_COLOR_BUFFER_BIT);
    g_procTable.glUseProgram(state->program);
    g_procTable.glActiveTexture(GL_TEXTURE0);
    g_procTable.glBindTexture(GL_TEXTURE_2D, state->sourceTextureId);
    g_procTable.glUniform1i(state->sourceLocation, 0);
    g_procTable.glUniform2f(state->sourceSizeLocation, GLfloat(sourceImage.width()), GLfloat(sourceImage.height()));
    g_procTable.glUniform2f(state->outputSizeLocation, GLfloat(targetSize.width()), GLfloat(targetSize.height()));
    g_procTable.glUniform1f(state->brightnessLocation, state->parameters.brightness);
    g_procTable.glUniform1f(state->contrastLocation, state->parameters.contrast);
    g_procTable.glUniform1f(state->zoomLocation, qMax(0.05f, state->parameters.zoom));
    g_procTable.glUniform2f(state->panLocation, state->parameters.panX * 0.65f, state->parameters.panY * 0.65f);
    g_procTable.glUniform1f(state->rotationLocation, qDegreesToRadians(state->parameters.rotationDegrees));
    g_procTable.glUniform2f(state->flipLocation,
                            state->parameters.flipHorizontal ? -1.0f : 1.0f,
                            state->parameters.flipVertical ? -1.0f : 1.0f);
    g_procTable.glUniform1i(state->heavyPassLocation, heavyPassCount);
    g_procTable.glVertexAttribPointer(GLuint(state->positionLocation), 2, GL_FLOAT, GL_FALSE, 0, kVertices);
    g_procTable.glEnableVertexAttribArray(GLuint(state->positionLocation));
    g_procTable.glVertexAttribPointer(GLuint(state->texCoordLocation), 2, GL_FLOAT, GL_FALSE, 0, kTexCoords);
    g_procTable.glEnableVertexAttribArray(GLuint(state->texCoordLocation));
    g_procTable.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g_procTable.glDisableVertexAttribArray(GLuint(state->positionLocation));
    g_procTable.glDisableVertexAttribArray(GLuint(state->texCoordLocation));
    g_procTable.glBindTexture(GL_TEXTURE_2D, 0);
    g_procTable.glUseProgram(0);
    g_procTable.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_procTable.glFlush();

    const GLenum finalError = g_procTable.glGetError();
    logSimulatorDiag(QStringLiteral("[diag] render end outputTex=%1 glError=%2")
                            .arg(state->outputTextureId)
                            .arg(glErrorHex(finalError)));
    if (finalError != GL_NO_ERROR) {
        if (error) {
            *error = QStringLiteral("photo_editor_render failed with GL error %1").arg(glErrorHex(finalError));
        }
        return false;
    }

    *textureId = state->outputTextureId;
    *size = targetSize;
    return true;
}
