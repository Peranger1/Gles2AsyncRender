#pragma once

#include <QMutex>
#include <QPointer>
#include <QSize>
#include <QSurfaceFormat>
#include <QThread>

class QOffscreenSurface;
class QOpenGLContext;

class SharedTextureWorker final : public QThread
{
    Q_OBJECT

public:
    SharedTextureWorker(QOpenGLContext *shareContext,
                        QOffscreenSurface *surface,
                        const QSurfaceFormat &format,
                        QObject *parent = nullptr);
    ~SharedTextureWorker() override;

    void setOutputSize(const QSize &size);
    void stop();

signals:
    void textureReady(quint32 textureId, QSize size, quint64 frameIndex);
    void initializationFailed(const QString &reason);
    void statusMessage(const QString &message);

protected:
    void run() override;

private:
    QSize currentOutputSize() const;

    QPointer<QOpenGLContext> m_shareContext;
    QPointer<QOffscreenSurface> m_surface;
    QSurfaceFormat m_format;

    mutable QMutex m_sizeMutex;
    QSize m_outputSize;
};
