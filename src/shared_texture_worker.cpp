#include "shared_texture_worker.h"

#include "angle_threading.h"
#include "gles_thread_guard.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QThread>
#include <QDebug>

#include <array>
#include <memory>

namespace
{
constexpr int kTargetFrameMs = 33;

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

        if (!ensureTextures(size, error)) {
            return false;
        }

        return true;
    }

    GLuint processFrame(const QSize &size, quint64 frameIndex, QString *error)
    {
        if (!ensureTextures(size, error)) {
            return 0U;
        }

        const GLuint texture = m_textures[static_cast<std::size_t>(frameIndex % m_textures.size())];

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
            m_textures = {0U, 0U};
        }

        m_program.removeAllShaders();
        m_allocatedSize = QSize();
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

    bool ensureTextures(const QSize &size, QString *error)
    {
        if (size.isEmpty()) {
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

        if (m_allocatedSize == size) {
            return true;
        }

        m_gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (GLuint texture : m_textures) {
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
        }
        m_gl->glBindTexture(GL_TEXTURE_2D, 0);

        const GLenum errorCode = m_gl->glGetError();
        if (errorCode != GL_NO_ERROR) {
            if (error) {
                *error = QStringLiteral("Texture allocation failed with GL error 0x%1")
                             .arg(static_cast<unsigned int>(errorCode), 0, 16);
            }
            return false;
        }

        m_allocatedSize = size;
        return true;
    }

    QOpenGLContext *m_context = nullptr;
    QOpenGLFunctions *m_gl = nullptr;
    QOpenGLShaderProgram m_program;
    GLuint m_fbo = 0U;
    std::array<GLuint, 2> m_textures = {0U, 0U};
    QSize m_allocatedSize;
    int m_positionLocation = -1;
    int m_texCoordLocation = -1;
    int m_timeLocation = -1;
    int m_resolutionLocation = -1;
};
} // namespace

SharedTextureWorker::SharedTextureWorker(QOpenGLContext *shareContext,
                                         QOffscreenSurface *surface,
                                         const QSurfaceFormat &format,
                                         QObject *parent)
    : QThread(parent)
    , m_shareContext(shareContext)
    , m_surface(surface)
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

    QOpenGLContext context;
    context.setFormat(m_format);
    context.setShareContext(m_shareContext.data());

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

    {
        ScopedGlesLock lock;
        if (!context.makeCurrent(m_surface.data())) {
            emit initializationFailed(QStringLiteral("Failed to make worker context current."));
            return;
        }

        const AngleThreadingInfo angleInfo = ensureAngleD3D11MultithreadProtection();
        emit statusMessage(angleInfo.message);

        if (!backend->initialize(&context, currentOutputSize(), &error)) {
            context.doneCurrent();
            emit initializationFailed(error);
            return;
        }

        context.doneCurrent();
    }

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

        GLuint texture = 0U;
        error.clear();

        {
            ScopedGlesLock lock;
            if (!context.makeCurrent(m_surface.data())) {
                emit initializationFailed(QStringLiteral("Failed to make worker context current for processing."));
                break;
            }

            texture = backend->processFrame(size, frameIndex, &error);
            if (texture == 0U) {
                context.doneCurrent();
                emit initializationFailed(error.isEmpty()
                                              ? QStringLiteral("The GPU algorithm backend returned an invalid texture.")
                                              : error);
                break;
            }

            // Cross-context handoff is conservative on ES2/ANGLE.
            context.functions()->glFinish();
            context.doneCurrent();
        }

        emit textureReady(texture, size, frameIndex);
        ++frameIndex;

        const qint64 elapsedMs = frameTimer.restart();
        if (elapsedMs < kTargetFrameMs) {
            msleep(static_cast<unsigned long>(kTargetFrameMs - elapsedMs));
        }
    }

    {
        ScopedGlesLock lock;
        if (context.makeCurrent(m_surface.data())) {
            backend->shutdown();
            context.doneCurrent();
        }
    }
}
