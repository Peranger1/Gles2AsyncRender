#pragma once

#include "async_render_executor.h"

#include <QObject>
#include <QMutex>
#include <QSize>
#include <memory>

class IAsyncRenderSession;
class IFramePublisher;
class IRenderRuntime;
class ISharedFrameSlotPool;

class AsyncRenderWorker final : public QObject
{
    Q_OBJECT

public:
    explicit AsyncRenderWorker(QObject *parent = nullptr);
    ~AsyncRenderWorker() override;

    bool configure(std::unique_ptr<IRenderRuntime> runtime,
                   std::unique_ptr<IAsyncRenderSession> session,
                   std::unique_ptr<IFramePublisher> publisher,
                   QString *error);

public slots:
    bool initialize(ISharedFrameSlotPool *slotPool);
    void submitLatestRequest(const AsyncRenderRequest &request);
    void requestRender();
    void onSlotAvailable();
    void shutdown();

signals:
    void frameReady(int slotIndex, quint64 generation, QSize size, quint64 frameIndex);
    void workerFailed(const QString &reason);
    void processingProgressChanged(int progress);

private slots:
    void onProcessProgressEvent(int progress, bool isEnd);

private:
    void schedulePump(int delayMs = 0);
    void pumpRender();

    std::unique_ptr<IRenderRuntime> m_runtime;
    std::unique_ptr<IAsyncRenderSession> m_session;
    std::unique_ptr<IFramePublisher> m_publisher;
    ISharedFrameSlotPool *m_slotPool = nullptr;
    AsyncRenderExecutor m_executor;
    bool m_configured = false;
    bool m_initialized = false;
    bool m_renderScheduled = false;
    bool m_shuttingDown = false;
    bool m_waitingForFreeSlot = false;
    quint64 m_frameIndex = 0;
    mutable QMutex m_stateMutex;
};
