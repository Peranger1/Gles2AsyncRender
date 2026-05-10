#pragma once

#include "image_effect_types.h"

#include <QObject>
#include <QMutex>
#include <QSize>
#include <memory>

class D3D11NativeSlotPool;

class D3D11NativeWorker final : public QObject
{
    Q_OBJECT

public:
    explicit D3D11NativeWorker(QObject *parent = nullptr);
    ~D3D11NativeWorker() override;

public slots:
    bool initialize(D3D11NativeSlotPool *slotPool, QSize outputSize);
    void setOutputSize(QSize size);
    void setEffectParameters(const ImageEffectParameters &parameters);
    void loadImageDirectory(const QString &directoryPath);
    void selectNextImage();
    void selectPreviousImage();
    void requestRender();
    void shutdown();

signals:
    void frameReady(int slotIndex, quint64 generation, QSize size, quint64 frameIndex);
    void initializationFailed(const QString &reason);
    void statusMessage(const QString &message);
    void imageDirectoryLoadFinished(bool loaded,
                                    const QString &errorMessage,
                                    int currentIndex,
                                    int count,
                                    const QString &displayName);
    void imageSelectionChanged(int currentIndex, int count, const QString &displayName);
    void renderTimingUpdated(double elapsedMs);

private:
    struct Impl;

    QSize currentOutputSize() const;
    void scheduleRender(int delayMs = 0);
    void emitImageSelection();

    std::unique_ptr<Impl> m_impl;
    D3D11NativeSlotPool *m_slotPool = nullptr;
    ImageEffectParameters m_effectParameters;
    QSize m_outputSize;
    bool m_initialized = false;
    bool m_renderScheduled = false;
    quint64 m_frameIndex = 0;
    mutable QMutex m_stateMutex;
};
