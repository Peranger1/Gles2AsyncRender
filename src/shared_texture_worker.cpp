#include "shared_texture_worker.h"

#include "angle_threading.h"
#include "gles_thread_guard.h"
#include "shared_texture_frame_pool.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QThread>
#include <QDebug>
#include <QScreen>

#include <array>
#include <memory>

namespace
{
constexpr int kTargetFrameMs = 33;
constexpr int kMakeCurrentRetryCount = 5;
constexpr unsigned long kMakeCurrentRetryDelayMs = 20;
constexpr unsigned long kNoFreeSlotSleepMs = 4;

constexpr GLfloat kVertices[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
    -1.0f,  1.0f,
     1.0f,  1.0f
};

constexpr GLfloat kTexCoords[] = {
    0.0f, 0.0f,
    1.0f, 0.0f,
    0.0f, 1.0f,
    1.0f, 1.0f
};

class DemoGpuAlgorithmBackend final
{
public:
    bool initialize(QOpenGLContext *context, const QSize &size, QString *error)
    {
        m_context = context;
        m_gl = context->functions();
        m_gl->initializeOpenGLFunctions();

        if (!createProgram(error)) {
            return false;
        }

        m_gl->glGenFramebuffers(1, &m_fbo);
        if (m_fbo == 0U) {
            if (error) {
                *error = QStringLiteral("Failed to create worker FBO.");
            }
            return false;
        }

        if (!ensureTextureForSlot(0, size, error)) {
            return false;
        }

        return true;
    }

    GLuint processFrame(int slotIndex, const QSize &size, quint64 frameIndex, QString *error)
    {
        if (slotIndex < 0 || slotIndex >= static_cast<int>(m_textures.size())) {
            if (error) {
                *error = QStringLiteral("Worker render slot index is out of range.");
            }
            return 0U;
        }

        if (!ensureTextureForSlot(slotIndex, size, error)) {
            return 0U;
        }

        const GLuint texture = m_textures[static_cast<std::size_t>(slotIndex)];

        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        m_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

        const GLenum framebufferStatus = m_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (framebufferStatus != GL_FRAMEBUFFER_COMPLETE) {
            if (error) {
                *error = QStringLiteral("Worker FBO is incomplete: 0x%1")
                             .arg(static_cast<unsigned int>(framebufferStatus), 0, 16);
            }
            m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return 0U;
        }

        const float time = static_cast<float>(frameIndex) * 0.033f;
        m_gl->glViewport(0, 0, size.width(), size.height());
        m_gl->glDisable(GL_DEPTH_TEST);
        m_gl->glClearColor(0.02f, 0.03f, 0.05f, 1.0f);
        m_gl->glClear(GL_COLOR_BUFFER_BIT);

        m_program.bind();
        m_gl->glUniform1f(m_timeLocation, time);
        m_gl->glUniform2f(m_resolutionLocation, static_cast<GLfloat>(size.width()), static_cast<GLfloat>(size.height()));

        m_gl->glVertexAttribPointer(m_positionLocation, 2, GL_FLOAT, GL_FALSE, 0, kVertices);
        m_gl->glEnableVertexAttribArray(m_positionLocation);
        m_gl->glVertexAttribPointer(m_texCoordLocation, 2, GL_FLOAT, GL_FALSE, 0, kTexCoords);
        m_gl->glEnableVertexAttribArray(m_texCoordLocation);

        m_gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        m_gl->glDisableVertexAttribArray(m_positionLocation);
        m_gl->glDisableVertexAttribArray(m_texCoordLocation);
        m_program.release();

        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return texture;
    }

    int textureCount() const
    {
        return static_cast<int>(m_textures.size());
    }

    GLuint textureIdForSlot(int slotIndex) const
    {
        if (slotIndex < 0 || slotIndex >= static_cast<int>(m_textures.size())) {
            return 0U;
        }
        return m_textures[static_cast<std::size_t>(slotIndex)];
    }

    void shutdown()
    {
        if (!m_gl) {
            return;
        }

        if (m_fbo != 0U) {
            m_gl->glDeleteFramebuffers(1, &m_fbo);
            m_fbo = 0U;
        }

        if (m_textures[0] != 0U) {
            m_gl->glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
            m_textures = {0U, 0U, 0U};
        }

        m_program.removeAllShaders();
        m_allocatedSizes = {};
        m_gl = nullptr;
        m_context = nullptr;
    }

private:
    bool createProgram(QString *error)
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
            precision mediump float;
            varying mediump vec2 vTexCoord;
            uniform mediump float uTime;
            uniform mediump vec2 uResolution;

            float hash(vec2 p)
            {
                return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
            }

            void main()
            {
                vec2 uv = vTexCoord;
                vec2 centered = (uv - 0.5) * vec2(uResolution.x / max(uResolution.y, 1.0), 1.0);
                float radial = length(centered);
                float angle = atan(centered.y, centered.x);

                float rings = sin(radial * 18.0 - uTime * 4.0);
                float swirl = cos(angle * 8.0 + radial * 22.0 - uTime * 2.1);
                float bands = sin((uv.x + uv.y) * 20.0 + uTime * 3.4);
                float noise = hash(floor(uv * 64.0) + floor(uTime * 4.0)) * 0.25;

                vec3 color;
                color.r = 0.45 + 0.35 * rings + 0.20 * swirl;
                color.g = 0.45 + 0.30 * swirl + 0.20 * bands;
                color.b = 0.40 + 0.25 * rings + 0.30 * bands + noise;

                gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
            }
        )";

        if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)) {
            if (error) {
                *error = QStringLiteral("Worker vertex shader compile failed: %1").arg(m_program.log());
            }
            return false;
        }

        if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)) {
            if (error) {
                *error = QStringLiteral("Worker fragment shader compile failed: %1").arg(m_program.log());
            }
            return false;
        }

        if (!m_program.link()) {
            if (error) {
                *error = QStringLiteral("Worker shader link failed: %1").arg(m_program.log());
            }
            return false;
        }

        m_positionLocation = m_program.attributeLocation("aPosition");
        m_texCoordLocation = m_program.attributeLocation("aTexCoord");
        m_timeLocation = m_program.uniformLocation("uTime");
        m_resolutionLocation = m_program.uniformLocation("uResolution");

        if (m_positionLocation < 0 || m_texCoordLocation < 0 || m_timeLocation < 0 || m_resolutionLocation < 0) {
            if (error) {
                *error = QStringLiteral("Worker shader locations are invalid.");
            }
            return false;
        }

        return true;
    }

    bool ensureTextureForSlot(int slotIndex, const QSize &size, QString *error)
    {
        if (size.isEmpty()) {
            return false;
        }

        if (slotIndex < 0 || slotIndex >= static_cast<int>(m_textures.size())) {
            if (error) {
                *error = QStringLiteral("Texture slot index is out of range.");
            }
            return false;
        }

        if (m_textures[0] == 0U) {
            m_gl->glGenTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
            if (m_textures[0] == 0U) {
                if (error) {
                    *error = QStringLiteral("Failed to create worker textures.");
                }
                return false;
            }
        }

        if (m_allocatedSizes[static_cast<std::size_t>(slotIndex)] == size) {
            return true;
        }

        m_gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const GLuint texture = m_textures[static_cast<std::size_t>(slotIndex)];
        m_gl->glBindTexture(GL_TEXTURE_2D, texture);
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

        const GLenum errorCode = m_gl->glGetError();
        if (errorCode != GL_NO_ERROR) {
            if (error) {
                *error = QStringLiteral("Texture allocation failed with GL error 0x%1")
                             .arg(static_cast<unsigned int>(errorCode), 0, 16);
            }
            return false;
        }

        m_allocatedSizes[static_cast<std::size_t>(slotIndex)] = size;
        return true;
    }

    QOpenGLContext *m_context = nullptr;
    QOpenGLFunctions *m_gl = nullptr;
    QOpenGLShaderProgram m_program;
    GLuint m_fbo = 0U;
    std::array<GLuint, 3> m_textures = {0U, 0U, 0U};
    std::array<QSize, 3> m_allocatedSizes;
    int m_positionLocation = -1;
    int m_texCoordLocation = -1;
    int m_timeLocation = -1;
    int m_resolutionLocation = -1;
};

QString formatToString(const QSurfaceFormat &format)
{
    QString text;
    QDebug debug(&text);
    debug.nospace() << format;
    return text;
}

QString threadIdToString(Qt::HANDLE threadId)
{
    return QStringLiteral("0x%1")
        .arg(quintptr(threadId), QT_POINTER_SIZE * 2, 16, QLatin1Char('0'));
}
} // namespace

SharedTextureWorker::SharedTextureWorker(QOpenGLContext *shareContext,
                                         QOffscreenSurface *surface,
                                         std::shared_ptr<SharedTextureFramePool> framePool,
                                         const QSurfaceFormat &format,
                                         QObject *parent)
    : QThread(parent)
    , m_shareContext(shareContext)
    , m_surface(surface)
    , m_framePool(std::move(framePool))
    , m_format(format)
{
}

SharedTextureWorker::~SharedTextureWorker()
{
    stop();
}

void SharedTextureWorker::setOutputSize(const QSize &size)
{
    QMutexLocker locker(&m_sizeMutex);
    m_outputSize = size.expandedTo(QSize(1, 1));
}

void SharedTextureWorker::stop()
{
    if (!isRunning()) {
        return;
    }

    requestInterruption();
    if (!wait(5000)) {
        qWarning() << "SharedTextureWorker did not stop within timeout.";
    }
}

QSize SharedTextureWorker::currentOutputSize() const
{
    QMutexLocker locker(&m_sizeMutex);
    return m_outputSize;
}

QString SharedTextureWorker::describeContextState(const QOpenGLContext *context) const
{
    const QOffscreenSurface *surface = m_surface.data();
    const QOpenGLContext *shareContext = m_shareContext.data();

    return QStringLiteral("workerThread=%1 contextValid=%2 contextFormat=%3 shareContext=%4 shareScreen=%5 "
                          "surfaceValid=%6 surfaceScreen=%7 surfaceRequestedFormat=%8 surfaceActualFormat=%9")
        .arg(threadIdToString(QThread::currentThreadId()),
             context && context->isValid() ? QStringLiteral("true") : QStringLiteral("false"),
             context ? formatToString(context->format()) : QStringLiteral("<null>"),
             shareContext ? QStringLiteral("present") : QStringLiteral("null"),
             shareContext && shareContext->screen() ? shareContext->screen()->name() : QStringLiteral("<null>"),
             surface && surface->isValid() ? QStringLiteral("true") : QStringLiteral("false"),
             surface && surface->screen() ? surface->screen()->name() : QStringLiteral("<null>"),
             surface ? formatToString(surface->requestedFormat()) : QStringLiteral("<null>"),
             surface ? formatToString(surface->format()) : QStringLiteral("<null>"));
}

bool SharedTextureWorker::makeWorkerContextCurrent(QOpenGLContext *context, const char *phase, QString *error)
{
    for (int attempt = 1; attempt <= kMakeCurrentRetryCount; ++attempt) {
        sharedGlesMutex().lock();
        if (context->makeCurrent(m_surface.data())) {
            return true;
        }
        sharedGlesMutex().unlock();

        qWarning().noquote()
            << QStringLiteral("Worker makeCurrent failed during %1 (attempt %2/%3). %4")
                   .arg(QString::fromLatin1(phase))
                   .arg(attempt)
                   .arg(kMakeCurrentRetryCount)
                   .arg(describeContextState(context));

        if (attempt < kMakeCurrentRetryCount) {
            msleep(kMakeCurrentRetryDelayMs);
        }
    }

    if (error) {
        *error = QStringLiteral("Failed to make worker context current during %1. %2")
                     .arg(QString::fromLatin1(phase), describeContextState(context));
    }
    return false;
}

void SharedTextureWorker::run()
{
    if (m_shareContext.isNull()) {
        emit initializationFailed(QStringLiteral("Share context is null."));
        return;
    }

    if (m_surface.isNull() || !m_surface->isValid()) {
        emit initializationFailed(QStringLiteral("Offscreen surface is not valid."));
        return;
    }

    if (!m_framePool || m_framePool->slotCount() <= 0) {
        emit initializationFailed(QStringLiteral("Shared frame pool is not available."));
        return;
    }

    QOpenGLContext context;
    context.setFormat(m_format);
    context.setShareContext(m_shareContext.data());
    if (m_shareContext->screen()) {
        context.setScreen(m_shareContext->screen());
    }

    {
        ScopedGlesLock lock;
        if (!context.create()) {
            emit initializationFailed(QStringLiteral("Failed to create worker OpenGL context."));
            return;
        }
    }

    qInfo() << "Worker context created. shareGroup=" << context.shareGroup()
            << "isOpenGLES=" << context.isOpenGLES()
            << "format=" << context.format();

    std::unique_ptr<DemoGpuAlgorithmBackend> backend = std::make_unique<DemoGpuAlgorithmBackend>();
    QString error;

    if (!makeWorkerContextCurrent(&context, "initialization", &error)) {
        emit initializationFailed(error);
        return;
    }

    const AngleThreadingInfo angleInfo = ensureAngleD3D11MultithreadProtection();
    emit statusMessage(angleInfo.message);

    if (!backend->initialize(&context, currentOutputSize(), &error)) {
        context.doneCurrent();
        sharedGlesMutex().unlock();
        emit initializationFailed(error);
        return;
    }

    m_framePool->reset();
    for (int i = 0; i < backend->textureCount(); ++i) {
        m_framePool->registerTexture(i, backend->textureIdForSlot(i));
    }

    context.doneCurrent();
    sharedGlesMutex().unlock();

    quint64 frameIndex = 0;
    QElapsedTimer frameTimer;
    frameTimer.start();

    while (!isInterruptionRequested()) {
        const QSize size = currentOutputSize();
        if (size.isEmpty()) {
            msleep(8);
            frameTimer.restart();
            continue;
        }

        int renderSlot = -1;
        if (!m_framePool->tryAcquireRenderSlot(&renderSlot)) {
            msleep(kNoFreeSlotSleepMs);
            continue;
        }

        GLuint texture = 0U;
        error.clear();

        if (!makeWorkerContextCurrent(&context, "processing", &error)) {
            m_framePool->abandonRenderSlot(renderSlot);
            emit initializationFailed(error);
            break;
        }

        texture = backend->processFrame(renderSlot, size, frameIndex, &error);
        if (texture == 0U) {
            context.doneCurrent();
            sharedGlesMutex().unlock();
            m_framePool->abandonRenderSlot(renderSlot);
            emit initializationFailed(error.isEmpty()
                                          ? QStringLiteral("The GPU algorithm backend returned an invalid texture.")
                                          : error);
            break;
        }

        // Cross-context handoff is conservative on ES2/ANGLE.
        context.functions()->glFinish();
        context.doneCurrent();
        sharedGlesMutex().unlock();

        SharedTextureFrame frame;
        if (m_framePool->submitRenderedFrame(renderSlot, size, frameIndex, &frame)) {
            emit textureReady(frame.slotIndex, frame.textureId, frame.size, frame.frameIndex);
        } else {
            m_framePool->abandonRenderSlot(renderSlot);
        }

        ++frameIndex;

        const qint64 elapsedMs = frameTimer.restart();
        if (elapsedMs < kTargetFrameMs) {
            msleep(static_cast<unsigned long>(kTargetFrameMs - elapsedMs));
        }
    }

    if (makeWorkerContextCurrent(&context, "shutdown", nullptr)) {
        backend->shutdown();
        context.doneCurrent();
        sharedGlesMutex().unlock();
    }

    m_framePool->reset();
}
