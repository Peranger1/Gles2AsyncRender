#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

#include <memory>

class QPaintEvent;
class QResizeEvent;
class SharedGlEnvironment;

struct SharedTextureFrame;
class SharedTextureFramePool;

class AsyncGlesWidget final : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit AsyncGlesWidget(QWidget *parent = nullptr);
    ~AsyncGlesWidget() override;

    void setSharedGlEnvironment(SharedGlEnvironment *environment);
    QSize outputPixelSize() const;
    SharedTextureFramePool *framePool() const;

protected:
    void initializeGL() override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

signals:
    void glInitialized();
    void outputSizeChanged(const QSize &outputSize);
    void displayReadyForWorker();

public slots:
    void onTextureReady(int slotIndex, quint32 textureId, QSize size, quint64 frameIndex);
    void onWorkerError(const QString &reason);
    void onWorkerStatus(const QString &message);

private slots:
    void lockForComposition();
    void unlockForComposition();
    void onAboutToCompose();
    void notifyDisplayReadyForWorker();
    void onFrameSwapped();

private:
    bool createProgram();

    QOpenGLShaderProgram m_program;
    int m_positionLocation = -1;
    int m_texCoordLocation = -1;
    int m_samplerLocation = -1;

    quint32 m_displayTexture = 0U;
    quint64 m_displayFrame = 0U;
    QSize m_displayTextureSize;
    int m_frontSlot = -1;
    int m_retiringSlot = -1;
    SharedTextureFrame *m_pendingFrame = nullptr;
    bool m_acceptFrames = false;
    bool m_workerReadyPending = false;
    bool m_compositionLocked = false;

    SharedGlEnvironment *m_sharedGlEnvironment = nullptr;
    std::shared_ptr<SharedTextureFramePool> m_framePool;
};
