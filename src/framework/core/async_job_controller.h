#pragma once

#include "async_pipeline.h"
#include "frame_publisher.h"
#include "work_runtime.h"
#include "work_scheduler.h"
#include "work_types.h"

#include <QObject>
#include <memory>

class IFrameWriter;
class IPlatformRenderBackend;

class AsyncJobController final : public QObject
{
    Q_OBJECT

public:
    explicit AsyncJobController(QObject *parent = nullptr);
    ~AsyncJobController() override;

    bool initialize(IPlatformRenderBackend *backend,
                    std::unique_ptr<IWorkProcessor> processor,
                    std::unique_ptr<IWorkScheduler> scheduler,
                    std::unique_ptr<IAsyncPipeline> pipeline,
                    QSize outputSize,
                    QString *error);
    bool initializeWithoutBackend(std::unique_ptr<IWorkRuntime> runtime,
                                  std::unique_ptr<IWorkProcessor> processor,
                                  std::unique_ptr<IWorkScheduler> scheduler,
                                  std::unique_ptr<IAsyncPipeline> pipeline,
                                  QString *error);

    bool isInitialized() const noexcept;
    QSize outputSize() const noexcept;
    void setOutputSize(QSize size);
    void submit(const WorkEnvelope &work);
    void requestPump();
    void onPublicationCapacityAvailable();
    void shutdown();

signals:
    void stateChanged(RequestId requestId, WorkState state);
    void progressChanged(RequestId requestId, int progress, bool isFinal);
    void messageEmitted(RequestId requestId, const QString &message);
    void resultReady(const JobResult &result);
    void fatalError(const QString &reason);

private:
    bool initializeInternal(std::unique_ptr<IWorkRuntime> runtime,
                            std::unique_ptr<IWorkProcessor> processor,
                            std::unique_ptr<IFramePublisher> publisher,
                            IFrameWriter *frameWriter,
                            std::unique_ptr<IWorkScheduler> scheduler,
                            std::unique_ptr<IAsyncPipeline> pipeline,
                            QSize outputSize,
                            QString *error);

    std::unique_ptr<IWorkRuntime> m_runtime;
    std::unique_ptr<IWorkProcessor> m_processor;
    std::unique_ptr<IFramePublisher> m_publisher;
    std::unique_ptr<IWorkScheduler> m_scheduler;
    std::unique_ptr<IAsyncPipeline> m_pipeline;
    IFrameWriter *m_frameWriter = nullptr;
    QSize m_outputSize;
    bool m_initialized = false;
    bool m_shuttingDown = false;
};
