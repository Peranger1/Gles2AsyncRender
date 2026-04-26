#pragma once

#include "image_effect_types.h"

#include <QObject>
#include <QMutex>
#include <QSize>
#include <QSurfaceFormat>
#include <QVector>
#include <QtGui/qopengl.h>

#include <memory>

class SharedGlContextHandle;
class SharedTextureFramePool;
class ImageProcessingPipeline;

class SharedTextureWorker final : public QObject
{
    Q_OBJECT

public:
    explicit SharedTextureWorker(QObject *parent = nullptr);
    ~SharedTextureWorker() override;

signals:
    void textureReady(int slotIndex, quint32 textureId, QSize size, quint64 frameIndex);
    void initializationFailed(const QString &reason);
    void statusMessage(const QString &message);
    void imageDirectoryLoadFinished(
        bool loaded,
        const QString &errorMessage,
        int currentIndex,
        int count,
        const QString &displayName);
    void imageSelectionChanged(int currentIndex, int count, const QString &displayName);

public slots:
    bool initialize(SharedGlContextHandle *handle,
                    SharedTextureFramePool *framePool,
                    QSize outputSize);
    void setOutputSize(QSize size);
    void loadImageDirectory(const QString &directoryPath);
    void selectNextImage();
    void selectPreviousImage();
    void setEffectParameters(const ImageEffectParameters &parameters);
    void requestRender();
    void shutdown();

private:
    QSize currentOutputSize() const;
    bool makeWorkerContextCurrent(const char *phase, QString *error);
    bool ensureSharedTextureForSlot(int slotIndex, const QSize &size, QString *error);
    QString describeContextState() const;
    void scheduleRender(int delayMs = 0);
    void emitImageSelection();

    SharedGlContextHandle *m_handle = nullptr;
    SharedTextureFramePool *m_framePool = nullptr;
    std::unique_ptr<ImageProcessingPipeline> m_pipeline;
    QVector<GLuint> m_sharedTextures;
    QVector<QSize> m_allocatedSizes;

    mutable QMutex m_stateMutex;
    QSize m_outputSize;
    ImageEffectParameters m_effectParameters;
    bool m_initialized = false;
    bool m_renderScheduled = false;
    quint64 m_frameIndex = 0;
};
