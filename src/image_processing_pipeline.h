#pragma once

#include "image_effect_types.h"

#include <QImage>
#include <QStringList>
#include <QtGui/qopengl.h>

class QOpenGLContext;
class QOpenGLFunctions;
class QOpenGLShaderProgram;

class ImageProcessingPipeline final
{
public:
    ImageProcessingPipeline() = default;
    ~ImageProcessingPipeline() = default;

    bool initialize(QOpenGLContext *context, QString *error);
    void release(QOpenGLContext *context);

    bool loadImageDirectory(QOpenGLContext *context, const QString &directoryPath, QString *error);
    bool selectNextImage();
    bool selectPreviousImage();
    bool hasImage() const;
    int imageCount() const;
    int currentIndex() const;
    QString currentDisplayName() const;

    bool renderToTexture(QOpenGLContext *context,
                         GLuint targetTexture,
                         const QSize &targetSize,
                         const ImageEffectParameters &parameters,
                         QString *error);

private:
    struct Vertex
    {
        float x = 0.0f;
        float y = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
    };

    bool ensureProgram(QString *error);
    bool ensureFramebuffer(QString *error);
    bool ensureSourceTexture(QOpenGLContext *context, QString *error);
    bool uploadCurrentImage(QOpenGLContext *context, QString *error);
    void updateGeometry(const ImageEffectParameters &parameters,
                        const QSize &contentSize,
                        const QSize &targetSize);
    QStringList collectImages(const QString &directoryPath) const;
    QSize sanitizedSize(const QSize &size) const;

    QOpenGLFunctions *m_gl = nullptr;
    QOpenGLShaderProgram *m_program = nullptr;
    GLuint m_framebuffer = 0U;
    GLuint m_sourceTexture = 0U;
    GLuint m_vertexBuffer = 0U;
    QStringList m_imagePaths;
    int m_currentIndex = -1;
    QImage m_currentImage;
    QSize m_uploadedImageSize;
    bool m_sourceDirty = false;
};
