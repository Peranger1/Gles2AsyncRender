#pragma once

#include "framework/core/async_render_types.h"
#include "image_effect_types.h"

#include <QObject>
#include <QMutex>
#include <QSize>
#include <memory>

class AsyncRenderWorker;
class ISharedFrameSlotPool;

class PhotoEditorAsyncRenderFacade final : public QObject
{
    Q_OBJECT

public:
    explicit PhotoEditorAsyncRenderFacade(QObject *parent = nullptr);
    ~PhotoEditorAsyncRenderFacade() override;

public slots:
    bool initialize(ISharedFrameSlotPool *slotPool, QSize outputSize);
    void setOutputSize(QSize size);
    void setEffectParameters(const ImageEffectParameters &parameters);
    void loadImageDirectory(const QString &directoryPath);
    void selectNextImage();
    void selectPreviousImage();
    void requestRender();
    void onSlotAvailable();
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
    struct ImageCatalogState;

    void emitImageSelection();
    void submitLatestRequest();
    AsyncRenderRequest buildLatestRequest() const;

    std::unique_ptr<ImageCatalogState> m_catalog;
    AsyncRenderWorker *m_worker = nullptr;
    ISharedFrameSlotPool *m_slotPool = nullptr;
    ImageEffectParameters m_effectParameters;
    QSize m_outputSize;
    bool m_initialized = false;
    bool m_shuttingDown = false;
    mutable quint64 m_requestSequence = 0;
    mutable QMutex m_stateMutex;
};
