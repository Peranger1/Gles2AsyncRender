#pragma once

#include "framework/platform/reader.h"

#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <deque>

class TexturePresentWidget final : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit TexturePresentWidget(QWidget *parent = nullptr);
    ~TexturePresentWidget() override;

    void setReader(IReader *reader);
    QSize outputPixelSize() const;
    quint64 outputRevision() const;
    void shutdown();

signals:
    void displayReady();
    void outputSizeChanged(QSize size, quint64 outputRevision);

public slots:
    void onTextureReady(const TextureTicket &ticket);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

private:
    bool createPrograms(QString *error);
    bool ensureDisplayTarget(const QSize &size, QString *error);
    bool copyLeaseToDisplayTexture(const TextureLease &lease, QString *error);
    void destroyDisplayTarget();

    IReader *m_reader = nullptr;
    QOpenGLShaderProgram m_program2D;
    QOpenGLShaderProgram m_programRect;
    int m_positionLocation2D = -1;
    int m_texCoordLocation2D = -1;
    int m_samplerLocation2D = -1;
    int m_positionLocationRect = -1;
    int m_texCoordLocationRect = -1;
    int m_samplerLocationRect = -1;
    bool m_rectSamplingSupported = false;
    GLuint m_displayTextureId = 0U;
    GLuint m_displayFramebufferId = 0U;
    QSize m_displayTextureSize;
    QSize m_displayContentSize;
    quint64 m_outputRevision = 0;
    std::deque<TextureTicket> m_pendingTickets;
    bool m_hasDisplayTexture = false;
};
