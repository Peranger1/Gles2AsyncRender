#pragma once

#include <QMutex>
#include <QPointer>
#include <QSize>
#include <QSurfaceFormat>
#include <QThread>

#include <memory>

class QOffscreenSurface;
class QOpenGLContext;
class SharedTextureFramePool;

class SharedTextureWorker final : public QThread
{
    Q_OBJECT

public:
    SharedTextureWorker(QOpenGLContext *shareContext,
                        QOffscreenSurface *surface,
                        std::shared_ptr<SharedTextureFramePool> framePool,
                        const QSurfaceFormat &format,
                        QObject *parent = nullptr);
    ~SharedTextureWorker() override;

    void setOutputSize(const QSize &size);
    void stop();

signals:
    void textureReady(int slotIndex, quint32 textureId, QSize size, quint64 frameIndex);
    void initializationFailed(const QString &reason);
    void statusMessage(const QString &message);

protected:
    void run() override;

private:
    QSize currentOutputSize() const;
    bool makeWorkerContextCurrent(QOpenGLContext *context, const char *phase, QString *error);
    QString describeContextState(const QOpenGLContext *context) const;

    QPointer<QOpenGLContext> m_shareContext;
    QPointer<QOffscreenSurface> m_surface;
    std::shared_ptr<SharedTextureFramePool> m_framePool;
    QSurfaceFormat m_format;

    mutable QMutex m_sizeMutex;
    QSize m_outputSize;
};
