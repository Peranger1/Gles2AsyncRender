#include "image_processing_pipeline.h"

#include <QDir>
#include <QFileInfo>
#include <QMatrix4x4>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QVector2D>
#include <QtMath>

namespace
{
constexpr int kVertexCount = 6;
}

bool ImageProcessingPipeline::initialize(QOpenGLContext *context, QString *error)
{
    if (context == nullptr || QOpenGLContext::currentContext() != context) {
        if (error) {
            *error = QStringLiteral("Image processing pipeline requires a current OpenGL context.");
        }
        return false;
    }

    m_gl = context->functions();
    m_gl->initializeOpenGLFunctions();

    if (!ensureProgram(error) || !ensureFramebuffer(error)) {
        return false;
    }

    if (m_vertexBuffer == 0U) {
        m_gl->glGenBuffers(1, &m_vertexBuffer);
        if (m_vertexBuffer == 0U) {
            if (error) {
                *error = QStringLiteral("Failed to create image processing vertex buffer.");
            }
            return false;
        }
    }

    return true;
}

void ImageProcessingPipeline::release(QOpenGLContext *context)
{
    if (context == nullptr || QOpenGLContext::currentContext() != context || m_gl == nullptr) {
        m_imagePaths.clear();
        m_currentIndex = -1;
        m_currentImage = QImage();
        m_uploadedImageSize = QSize();
        m_sourceDirty = false;
        return;
    }

    if (m_sourceTexture != 0U) {
        m_gl->glDeleteTextures(1, &m_sourceTexture);
        m_sourceTexture = 0U;
    }

    if (m_framebuffer != 0U) {
        m_gl->glDeleteFramebuffers(1, &m_framebuffer);
        m_framebuffer = 0U;
    }

    if (m_vertexBuffer != 0U) {
        m_gl->glDeleteBuffers(1, &m_vertexBuffer);
        m_vertexBuffer = 0U;
    }

    delete m_program;
    m_program = nullptr;
    m_gl = nullptr;
    m_imagePaths.clear();
    m_currentIndex = -1;
    m_currentImage = QImage();
    m_uploadedImageSize = QSize();
    m_sourceDirty = false;
}

bool ImageProcessingPipeline::loadImageDirectory(QOpenGLContext *context, const QString &directoryPath, QString *error)
{
    if (!initialize(context, error)) {
        return false;
    }

    const QStringList imagePaths = collectImages(directoryPath);
    if (imagePaths.isEmpty()) {
        if (error) {
            *error = QStringLiteral("No supported images were found in the selected directory.");
        }
        return false;
    }

    m_imagePaths = imagePaths;
    m_currentIndex = 0;
    m_currentImage = QImage(m_imagePaths.first()).convertToFormat(QImage::Format_RGBA8888);
    m_sourceDirty = true;

    if (m_currentImage.isNull()) {
        if (error) {
            *error = QStringLiteral("Failed to load the first image from the selected directory.");
        }
        return false;
    }

    return ensureSourceTexture(context, error);
}

bool ImageProcessingPipeline::selectNextImage()
{
    if (m_imagePaths.isEmpty()) {
        return false;
    }

    m_currentIndex = (m_currentIndex + 1) % m_imagePaths.size();
    m_currentImage = QImage(m_imagePaths[m_currentIndex]).convertToFormat(QImage::Format_RGBA8888);
    m_sourceDirty = true;
    return !m_currentImage.isNull();
}

bool ImageProcessingPipeline::selectPreviousImage()
{
    if (m_imagePaths.isEmpty()) {
        return false;
    }

    m_currentIndex = (m_currentIndex - 1 + m_imagePaths.size()) % m_imagePaths.size();
    m_currentImage = QImage(m_imagePaths[m_currentIndex]).convertToFormat(QImage::Format_RGBA8888);
    m_sourceDirty = true;
    return !m_currentImage.isNull();
}

bool ImageProcessingPipeline::hasImage() const
{
    return !m_imagePaths.isEmpty() && !m_currentImage.isNull();
}

int ImageProcessingPipeline::imageCount() const
{
    return m_imagePaths.size();
}

int ImageProcessingPipeline::currentIndex() const
{
    return m_currentIndex;
}

QString ImageProcessingPipeline::currentDisplayName() const
{
    if (m_currentIndex < 0 || m_currentIndex >= m_imagePaths.size()) {
        return {};
    }

    return QFileInfo(m_imagePaths[m_currentIndex]).fileName();
}

bool ImageProcessingPipeline::renderToTexture(QOpenGLContext *context,
                                              GLuint targetTexture,
                                              const QSize &targetSize,
                                              const ImageEffectParameters &parameters,
                                              QString *error)
{
    if (!initialize(context, error)) {
        return false;
    }

    if (!hasImage()) {
        if (error) {
            *error = QStringLiteral("No image is currently loaded.");
        }
        return false;
    }

    if (!ensureSourceTexture(context, error)) {
        return false;
    }

    const QSize safeTargetSize = sanitizedSize(targetSize);
    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    m_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTexture, 0);

    const GLenum fboStatus = m_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (error) {
            *error = QStringLiteral("Image processing framebuffer is incomplete: 0x%1")
                         .arg(static_cast<unsigned int>(fboStatus), 0, 16);
        }
        return false;
    }

    updateGeometry(parameters, m_currentImage.size(), safeTargetSize);

    m_gl->glViewport(0, 0, safeTargetSize.width(), safeTargetSize.height());
    m_gl->glDisable(GL_DEPTH_TEST);
    m_gl->glDisable(GL_BLEND);
    m_gl->glClearColor(0.07f, 0.08f, 0.10f, 1.0f);
    m_gl->glClear(GL_COLOR_BUFFER_BIT);

    m_program->bind();
    m_program->setUniformValue("u_texture", 0);
    m_program->setUniformValue("u_brightness", parameters.brightness);
    m_program->setUniformValue("u_contrast", parameters.contrast);

    m_gl->glActiveTexture(GL_TEXTURE0);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_sourceTexture);

    m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    const int positionLocation = m_program->attributeLocation("a_position");
    const int texCoordLocation = m_program->attributeLocation("a_texCoord");
    m_program->enableAttributeArray(positionLocation);
    m_program->enableAttributeArray(texCoordLocation);
    m_program->setAttributeBuffer(positionLocation, GL_FLOAT, offsetof(Vertex, x), 2, sizeof(Vertex));
    m_program->setAttributeBuffer(texCoordLocation, GL_FLOAT, offsetof(Vertex, u), 2, sizeof(Vertex));

    m_gl->glDrawArrays(GL_TRIANGLES, 0, kVertexCount);

    m_program->disableAttributeArray(positionLocation);
    m_program->disableAttributeArray(texCoordLocation);
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_gl->glBindTexture(GL_TEXTURE_2D, 0);
    m_program->release();
    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return m_gl->glGetError() == GL_NO_ERROR;
}

bool ImageProcessingPipeline::ensureProgram(QString *error)
{
    if (m_program != nullptr) {
        return true;
    }

    m_program = new QOpenGLShaderProgram();
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex,
            "attribute vec2 a_position;\n"
            "attribute vec2 a_texCoord;\n"
            "varying vec2 v_texCoord;\n"
            "void main()\n"
            "{\n"
            "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
            "    v_texCoord = a_texCoord;\n"
            "}\n")) {
        if (error) {
            *error = QStringLiteral("Image vertex shader compile failed: %1").arg(m_program->log());
        }
        return false;
    }

    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#ifdef GL_ES\n"
            "precision mediump float;\n"
            "#endif\n"
            "uniform sampler2D u_texture;\n"
            "uniform float u_brightness;\n"
            "uniform float u_contrast;\n"
            "varying vec2 v_texCoord;\n"
            "void main()\n"
            "{\n"
            "    vec4 color = texture2D(u_texture, v_texCoord);\n"
            "    color.rgb = (color.rgb - vec3(0.5)) * u_contrast + vec3(0.5 + u_brightness);\n"
            "    color.rgb = clamp(color.rgb, 0.0, 1.0);\n"
            "    gl_FragColor = color;\n"
            "}\n")) {
        if (error) {
            *error = QStringLiteral("Image fragment shader compile failed: %1").arg(m_program->log());
        }
        return false;
    }

    if (!m_program->link()) {
        if (error) {
            *error = QStringLiteral("Image shader link failed: %1").arg(m_program->log());
        }
        return false;
    }

    return true;
}

bool ImageProcessingPipeline::ensureFramebuffer(QString *error)
{
    if (m_framebuffer != 0U) {
        return true;
    }

    m_gl->glGenFramebuffers(1, &m_framebuffer);
    if (m_framebuffer == 0U) {
        if (error) {
            *error = QStringLiteral("Failed to create image processing framebuffer.");
        }
        return false;
    }

    return true;
}

bool ImageProcessingPipeline::ensureSourceTexture(QOpenGLContext *context, QString *error)
{
    if (m_sourceTexture == 0U) {
        m_gl->glGenTextures(1, &m_sourceTexture);
        if (m_sourceTexture == 0U) {
            if (error) {
                *error = QStringLiteral("Failed to create source image texture.");
            }
            return false;
        }
        m_sourceDirty = true;
    }

    if (!m_sourceDirty) {
        return true;
    }

    return uploadCurrentImage(context, error);
}

bool ImageProcessingPipeline::uploadCurrentImage(QOpenGLContext *context, QString *error)
{
    Q_UNUSED(context)

    if (m_gl == nullptr || m_sourceTexture == 0U || m_currentImage.isNull()) {
        if (error) {
            *error = QStringLiteral("Source image upload prerequisites are not ready.");
        }
        return false;
    }

    m_gl->glBindTexture(GL_TEXTURE_2D, m_sourceTexture);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    m_gl->glTexImage2D(GL_TEXTURE_2D,
                       0,
                       GL_RGBA,
                       m_currentImage.width(),
                       m_currentImage.height(),
                       0,
                       GL_RGBA,
                       GL_UNSIGNED_BYTE,
                       m_currentImage.constBits());
    m_gl->glBindTexture(GL_TEXTURE_2D, 0);

    const GLenum glError = m_gl->glGetError();
    if (glError != GL_NO_ERROR) {
        if (error) {
            *error = QStringLiteral("Source image upload failed with GL error 0x%1")
                         .arg(static_cast<unsigned int>(glError), 0, 16);
        }
        return false;
    }

    m_uploadedImageSize = m_currentImage.size();
    m_sourceDirty = false;
    return true;
}

void ImageProcessingPipeline::updateGeometry(const ImageEffectParameters &parameters,
                                             const QSize &contentSize,
                                             const QSize &targetSize)
{
    const QSize safeContentSize = sanitizedSize(contentSize);
    const QSize safeTargetSize = sanitizedSize(targetSize);
    const float contentAspect = float(safeContentSize.width()) / float(safeContentSize.height());
    const float targetAspect = float(safeTargetSize.width()) / float(safeTargetSize.height());

    float halfWidth = 1.0f;
    float halfHeight = 1.0f;
    if (contentAspect > targetAspect) {
        halfHeight = targetAspect / contentAspect;
    } else {
        halfWidth = contentAspect / targetAspect;
    }

    const float safeZoom = qMax(0.05f, parameters.zoom);
    const float angleRadians = qDegreesToRadians(parameters.rotationDegrees);
    const float cosAngle = qCos(angleRadians);
    const float sinAngle = qSin(angleRadians);
    const float scaleX = parameters.flipHorizontal ? -safeZoom : safeZoom;
    const float scaleY = parameters.flipVertical ? -safeZoom : safeZoom;

    const QVector2D baseCorners[4] = {
        QVector2D(-halfWidth, halfHeight),
        QVector2D(halfWidth, halfHeight),
        QVector2D(-halfWidth, -halfHeight),
        QVector2D(halfWidth, -halfHeight)
    };
    const QVector2D texCoords[4] = {
        QVector2D(0.0f, 1.0f),
        QVector2D(1.0f, 1.0f),
        QVector2D(0.0f, 0.0f),
        QVector2D(1.0f, 0.0f)
    };

    QVector2D transformed[4];
    for (int i = 0; i < 4; ++i) {
        const float scaledX = baseCorners[i].x() * scaleX;
        const float scaledY = baseCorners[i].y() * scaleY;
        const float rotatedX = (scaledX * cosAngle) - (scaledY * sinAngle);
        const float rotatedY = (scaledX * sinAngle) + (scaledY * cosAngle);
        transformed[i] = QVector2D(rotatedX + parameters.panX, rotatedY + parameters.panY);
    }

    const Vertex vertices[kVertexCount] = {
        {transformed[0].x(), transformed[0].y(), texCoords[0].x(), texCoords[0].y()},
        {transformed[1].x(), transformed[1].y(), texCoords[1].x(), texCoords[1].y()},
        {transformed[2].x(), transformed[2].y(), texCoords[2].x(), texCoords[2].y()},
        {transformed[2].x(), transformed[2].y(), texCoords[2].x(), texCoords[2].y()},
        {transformed[1].x(), transformed[1].y(), texCoords[1].x(), texCoords[1].y()},
        {transformed[3].x(), transformed[3].y(), texCoords[3].x(), texCoords[3].y()}
    };

    m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    m_gl->glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

QStringList ImageProcessingPipeline::collectImages(const QString &directoryPath) const
{
    QDir directory(directoryPath);
    const QStringList filters = {
        QStringLiteral("*.png"),
        QStringLiteral("*.jpg"),
        QStringLiteral("*.jpeg"),
        QStringLiteral("*.bmp"),
        QStringLiteral("*.webp")
    };
    QStringList result;
    const QFileInfoList entries = directory.entryInfoList(
        filters,
        QDir::Files | QDir::Readable | QDir::NoSymLinks,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        result.push_back(entry.absoluteFilePath());
    }
    return result;
}

QSize ImageProcessingPipeline::sanitizedSize(const QSize &size) const
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}
