#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

#include <memory>

class QOffscreenSurface;
class QPaintEvent;
class QResizeEvent;

struct SharedTextureFrame;
class SharedTextureFramePool;
class SharedTextureWorker;

class AsyncGlesWidget final : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit AsyncGlesWidget(QWidget *parent = nullptr);
    ~AsyncGlesWidget() override;

protected:
    void initializeGL() override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private slots:
    void onTextureReady(int slotIndex, quint32 textureId, QSize size, quint64 frameIndex);
    void onWorkerError(const QString &reason);
    void onWorkerStatus(const QString &message);
    void startWorkerIfNeeded();
    void onFrameSwapped();

private:
    QSize outputPixelSize() const;
    bool createProgram();
    void lockForComposition();
    void unlockForComposition();
    void stopWorker();

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
    bool m_compositionLocked = false;
    bool m_workerStartPending = false;

    std::unique_ptr<QOffscreenSurface> m_surface;
    std::shared_ptr<SharedTextureFramePool> m_framePool;
    std::unique_ptr<SharedTextureWorker> m_worker;
};
