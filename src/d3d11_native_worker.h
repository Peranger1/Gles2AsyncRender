#pragma once

#include "image_effect_types.h"

#include <QObject>
#include <QMutex>
#include <QSize>
#include <memory>

class ISharedFrameSlotPool;

class D3D11NativeWorker final : public QObject
{
    Q_OBJECT

public:
    explicit D3D11NativeWorker(QObject *parent = nullptr);
    ~D3D11NativeWorker() override;

public slots:
    bool initialize(ISharedFrameSlotPool *slotPool, QSize outputSize);
    void setOutputSize(QSize size);
    void setEffectParameters(const ImageEffectParameters &parameters);
    void loadImageDirectory(const QString &directoryPath);
    void selectNextImage();
    void selectPreviousImage();
    void requestRender();
    void onSlotAvailableForWorker();
    void shutdown();

signals:
    void frameReady(int slotIndex, quint64 generation, QSize size, quint64 frameIndex);
    void initializationFailed(const QString &reason);
    void imageDirectoryLoadFinished(bool loaded,
                                    const QString &errorMessage,
                                    int currentIndex,
                                    int count,
                                    const QString &displayName,
                                    QSize imageSize);
    void imageSelectionChanged(int currentIndex, int count, const QString &displayName, QSize imageSize);
    void processingProgressChanged(int progress);

private:
    struct Impl;

    QSize currentOutputSize() const;
    ImageEffectParameters currentEffectParameters() const;
    void enqueueLatestRenderRequest();
    void schedulePump(int delayMs = 0);
    void emitImageSelection();
    void pumpRender();

private slots:
    void onProcessProgressEvent(int progress, bool isEnd);

private:
    std::unique_ptr<Impl> m_impl;
    ISharedFrameSlotPool *m_slotPool = nullptr;
    ImageEffectParameters m_effectParameters;
    QSize m_outputSize;
    bool m_initialized = false;
    bool m_renderScheduled = false;
    bool m_shuttingDown = false;
    bool m_waitingForFreeSlot = false;
    quint64 m_frameIndex = 0;
    quint64 m_requestSequence = 0;
    mutable QMutex m_stateMutex;
};
