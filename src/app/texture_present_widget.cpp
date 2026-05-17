#include "texture_present_widget.h"

#include "runtime_diagnostics.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <array>

namespace
{
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

void logWidgetMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[TexturePresentWidget]", message);
}

bool isExpiredTicketError(const QString &message)
{
    return message.contains(QStringLiteral("no longer ready for reading"), Qt::CaseInsensitive);
}
}

TexturePresentWidget::TexturePresentWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
}

TexturePresentWidget::~TexturePresentWidget()
{
    shutdown();
}

void TexturePresentWidget::setReader(IReader *reader)
{
    m_reader = reader;
}

void TexturePresentWidget::shutdown()
{
    if (m_reader == nullptr && m_displayTextureId == 0U && m_displayFramebufferId == 0U) {
        return;
    }

    if (context()) {
        makeCurrent();
        if (m_reader) {
            m_reader->detach();
        }
        destroyDisplayTarget();
        doneCurrent();
    }

    m_reader = nullptr;
    m_pendingTickets.clear();
    m_displayTextureSize = QSize();
    m_displayContentSize = QSize();
    m_hasDisplayTexture = false;
}

QSize TexturePresentWidget::outputPixelSize() const
{
    const qreal dpr = devicePixelRatioF();
    return QSize(qMax(1, qRound(width() * dpr)),
                 qMax(1, qRound(height() * dpr)));
}

quint64 TexturePresentWidget::outputRevision() const
{
    return m_outputRevision;
}

void TexturePresentWidget::onTextureReady(const TextureTicket &ticket)
{
    if (ticket.outputRevision != m_outputRevision) {
        return;
    }

    m_pendingTickets.push_back(ticket);
    update();
}

void TexturePresentWidget::initializeGL()
{
    QOpenGLFunctions *gl = context() ? context()->functions() : nullptr;
    if (gl == nullptr) {
        logWidgetMessage(QStringLiteral("Widget initialization failed: QOpenGLFunctions is unavailable."));
        return;
    }

    gl->glDisable(GL_DEPTH_TEST);
    gl->glClearColor(0.05f, 0.06f, 0.08f, 1.0f);

    QString error;
    if (!createPrograms(&error)) {
        logWidgetMessage(error);
        return;
    }
    if (m_reader && !m_reader->attachToCurrentContext(&error)) {
        logWidgetMessage(error);
        return;
    }

    emit displayReady();
}

void TexturePresentWidget::resizeGL(int width, int height)
{
    Q_UNUSED(width);
    Q_UNUSED(height);
    ++m_outputRevision;
    m_pendingTickets.clear();
    emit outputSizeChanged(outputPixelSize(), m_outputRevision);
}

void TexturePresentWidget::paintGL()
{
    QOpenGLFunctions *gl = context() ? context()->functions() : nullptr;
    if (gl == nullptr) {
        return;
    }

    const QSize viewportSize = outputPixelSize();
    gl->glViewport(0, 0, viewportSize.width(), viewportSize.height());
    gl->glClear(GL_COLOR_BUFFER_BIT);

    bool displayedTicket = false;
    while (!m_pendingTickets.empty() && m_reader) {
        const TextureTicket ticket = m_pendingTickets.front();
        m_pendingTickets.pop_front();
        TextureLease lease;
        QString error;
        if (m_reader->acquire(ticket, &lease, &error)) {
            if (copyLeaseToDisplayTexture(lease, &error)) {
                m_displayContentSize = lease.size;
                m_hasDisplayTexture = true;
                displayedTicket = true;
            } else if (!error.isEmpty()) {
                logWidgetMessage(error);
            }
            m_reader->release(lease);
            break;
        }

        if (!error.isEmpty() && !isExpiredTicketError(error)) {
            logWidgetMessage(error);
            break;
        }
    }

    if (displayedTicket && !m_pendingTickets.empty()) {
        update();
    }

    if (!m_hasDisplayTexture) {
        return;
    }

    m_program2D.bind();
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, m_displayTextureId);
    gl->glUniform1i(m_samplerLocation2D, 0);
    const std::array<GLfloat, 8> vertices = aspectFitVertices(m_displayContentSize, viewportSize);
    gl->glVertexAttribPointer(m_positionLocation2D, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
    gl->glEnableVertexAttribArray(m_positionLocation2D);
    gl->glVertexAttribPointer(m_texCoordLocation2D, 2, GL_FLOAT, GL_FALSE, 0, kDisplayTexCoords);
    gl->glEnableVertexAttribArray(m_texCoordLocation2D);
    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl->glDisableVertexAttribArray(m_positionLocation2D);
    gl->glDisableVertexAttribArray(m_texCoordLocation2D);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    m_program2D.release();
}

bool TexturePresentWidget::createPrograms(QString *error)
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

    static const char *fragmentShader2D = R"(
        varying mediump vec2 vTexCoord;
        uniform sampler2D uTexture;

        void main()
        {
            gl_FragColor = texture2D(uTexture, vTexCoord);
        }
    )";

    static const char *fragmentShaderRect = R"(
        #extension GL_ARB_texture_rectangle : enable
        varying mediump vec2 vTexCoord;
        uniform sampler2DRect uTexture;

        void main()
        {
            gl_FragColor = texture2DRect(uTexture, vTexCoord);
        }
    )";

    if (!m_program2D.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)) {
        if (error) {
            *error = m_program2D.log();
        }
        return false;
    }

    if (!m_program2D.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader2D)) {
        if (error) {
            *error = m_program2D.log();
        }
        return false;
    }

    if (!m_program2D.link()) {
        if (error) {
            *error = m_program2D.log();
        }
        return false;
    }

    m_positionLocation2D = m_program2D.attributeLocation("aPosition");
    m_texCoordLocation2D = m_program2D.attributeLocation("aTexCoord");
    m_samplerLocation2D = m_program2D.uniformLocation("uTexture");
    if (m_positionLocation2D < 0 || m_texCoordLocation2D < 0 || m_samplerLocation2D < 0) {
        if (error) {
            *error = QStringLiteral("The 2D display shader program is missing required attributes or uniforms.");
        }
        return false;
    }

    m_rectSamplingSupported = false;
    if (context() && context()->format().renderableType() != QSurfaceFormat::OpenGLES) {
        if (!m_programRect.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)) {
            if (error) {
                *error = m_programRect.log();
            }
            return false;
        }

        if (!m_programRect.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderRect)) {
            if (error) {
                *error = m_programRect.log();
            }
            return false;
        }

        if (!m_programRect.link()) {
            if (error) {
                *error = m_programRect.log();
            }
            return false;
        }

        m_positionLocationRect = m_programRect.attributeLocation("aPosition");
        m_texCoordLocationRect = m_programRect.attributeLocation("aTexCoord");
        m_samplerLocationRect = m_programRect.uniformLocation("uTexture");
        m_rectSamplingSupported = m_positionLocationRect >= 0 && m_texCoordLocationRect >= 0 && m_samplerLocationRect >= 0;
        if (!m_rectSamplingSupported) {
            if (error) {
                *error = QStringLiteral("The rectangle-texture display shader program is missing required attributes or uniforms.");
            }
            return false;
        }
    }

    return true;
}

bool TexturePresentWidget::ensureDisplayTarget(const QSize &size, QString *error)
{
    QOpenGLFunctions *gl = context() ? context()->functions() : nullptr;
    if (gl == nullptr || !size.isValid()) {
        if (error) {
            *error = QStringLiteral("TexturePresentWidget cannot allocate the local display target.");
        }
        return false;
    }
    if (m_displayTextureId != 0U && m_displayFramebufferId != 0U && m_displayTextureSize == size) {
        return true;
    }

    destroyDisplayTarget();

    gl->glGenTextures(1, &m_displayTextureId);
    gl->glBindTexture(GL_TEXTURE_2D, m_displayTextureId);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA,
                     size.width(),
                     size.height(),
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     nullptr);
    gl->glBindTexture(GL_TEXTURE_2D, 0);

    gl->glGenFramebuffers(1, &m_displayFramebufferId);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, m_displayFramebufferId);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_displayTextureId, 0);
    const GLenum status = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error) {
            *error = QStringLiteral("The widget display framebuffer is incomplete: 0x%1").arg(unsigned(status), 0, 16);
        }
        destroyDisplayTarget();
        return false;
    }

    m_displayTextureSize = size;
    return true;
}

bool TexturePresentWidget::copyLeaseToDisplayTexture(const TextureLease &lease, QString *error)
{
    QOpenGLFunctions *gl = context() ? context()->functions() : nullptr;
    if (gl == nullptr || lease.textureId == 0U || !ensureDisplayTarget(lease.size, error)) {
        return false;
    }

    static const GLfloat kVertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };

    static const GLfloat kImportedTexCoords[] = {
        0.0f, 1.0f,
        1.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f
    };

    const bool isRectTarget = lease.textureTarget == GL_TEXTURE_RECTANGLE_ARB;
    if (isRectTarget && !m_rectSamplingSupported) {
        if (error) {
            *error = QStringLiteral("The current display context does not support rectangle-texture sampling.");
        }
        return false;
    }
    GLfloat importedTexCoords[8] = {
        kImportedTexCoords[0], kImportedTexCoords[1],
        kImportedTexCoords[2], kImportedTexCoords[3],
        kImportedTexCoords[4], kImportedTexCoords[5],
        kImportedTexCoords[6], kImportedTexCoords[7]
    };
    QOpenGLShaderProgram *program = &m_program2D;
    int positionLocation = m_positionLocation2D;
    int texCoordLocation = m_texCoordLocation2D;
    int samplerLocation = m_samplerLocation2D;
    if (isRectTarget) {
        importedTexCoords[0] = 0.0f;
        importedTexCoords[1] = GLfloat(lease.size.height());
        importedTexCoords[2] = GLfloat(lease.size.width());
        importedTexCoords[3] = GLfloat(lease.size.height());
        importedTexCoords[4] = 0.0f;
        importedTexCoords[5] = 0.0f;
        importedTexCoords[6] = GLfloat(lease.size.width());
        importedTexCoords[7] = 0.0f;
        program = &m_programRect;
        positionLocation = m_positionLocationRect;
        texCoordLocation = m_texCoordLocationRect;
        samplerLocation = m_samplerLocationRect;
    }

    gl->glBindFramebuffer(GL_FRAMEBUFFER, m_displayFramebufferId);
    gl->glViewport(0, 0, lease.size.width(), lease.size.height());
    program->bind();
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(lease.textureTarget, lease.textureId);
    gl->glUniform1i(samplerLocation, 0);
    gl->glVertexAttribPointer(positionLocation, 2, GL_FLOAT, GL_FALSE, 0, kVertices);
    gl->glEnableVertexAttribArray(positionLocation);
    gl->glVertexAttribPointer(texCoordLocation, 2, GL_FLOAT, GL_FALSE, 0, importedTexCoords);
    gl->glEnableVertexAttribArray(texCoordLocation);
    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl->glDisableVertexAttribArray(positionLocation);
    gl->glDisableVertexAttribArray(texCoordLocation);
    gl->glBindTexture(lease.textureTarget, 0);
    program->release();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glFlush();

    const GLenum glError = gl->glGetError();
    if (glError != GL_NO_ERROR) {
        if (error) {
            *error = QStringLiteral("The widget copy pass failed with GL error 0x%1").arg(unsigned(glError), 0, 16);
        }
        return false;
    }
    return true;
}

void TexturePresentWidget::destroyDisplayTarget()
{
    QOpenGLFunctions *gl = context() ? context()->functions() : nullptr;
    if (gl == nullptr) {
        return;
    }

    if (m_displayFramebufferId != 0U) {
        gl->glDeleteFramebuffers(1, &m_displayFramebufferId);
        m_displayFramebufferId = 0U;
    }
    if (m_displayTextureId != 0U) {
        gl->glDeleteTextures(1, &m_displayTextureId);
        m_displayTextureId = 0U;
    }
    m_displayTextureSize = QSize();
    m_displayContentSize = QSize();
    m_hasDisplayTexture = false;
}
