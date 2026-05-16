#pragma once

#include "framework/platform/reader.h"

#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

class TexturePresentWidget final : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit TexturePresentWidget(QWidget *parent = nullptr);
    ~TexturePresentWidget() override;

    void setReader(IReader *reader);
    QSize outputPixelSize() const;
    void shutdown();

signals:
    void displayReady();
    void outputSizeChanged(QSize size);
    void textureConsumed();

public slots:
    void onTextureReady(const TextureTicket &ticket);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

private:
    bool createProgram(QString *error);
    bool ensureDisplayTarget(const QSize &size, QString *error);
    bool copyLeaseToDisplayTexture(const TextureLease &lease, QString *error);
    void destroyDisplayTarget();

    IReader *m_reader = nullptr;
    QOpenGLShaderProgram m_program;
    int m_positionLocation = -1;
    int m_texCoordLocation = -1;
    int m_samplerLocation = -1;
    GLuint m_displayTextureId = 0U;
    GLuint m_displayFramebufferId = 0U;
    QSize m_displayTextureSize;
    QSize m_displayContentSize;
    TextureTicket m_pendingTicket;
    bool m_hasPendingTicket = false;
    bool m_hasDisplayTexture = false;
};
