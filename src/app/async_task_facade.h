#pragma once

#include "framework/core/request_coalescer.h"
#include "framework/core/work_processor.h"
#include "framework/core/work_runtime.h"
#include "framework/core/work_types.h"

#include <QObject>
#include <QSize>
#include <memory>

class AsyncJobController;
class IPlatformRenderBackend;

class AsyncTaskFacade final : public QObject
{
    Q_OBJECT

public:
    explicit AsyncTaskFacade(QObject *parent = nullptr);
    ~AsyncTaskFacade() override;

    bool initialize(IPlatformRenderBackend *backend,
                    std::unique_ptr<IWorkProcessor> processor,
                    QSize outputSize,
                    QString *error,
                    std::shared_ptr<IRequestCoalescer> coalescer = {});
    bool initializeWithoutBackend(std::unique_ptr<IWorkRuntime> runtime,
                                  std::unique_ptr<IWorkProcessor> processor,
                                  QString *error,
                                  std::shared_ptr<IRequestCoalescer> coalescer = {});

    bool isInitialized() const noexcept;
    QSize outputSize() const noexcept;

public slots:
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
    std::unique_ptr<AsyncJobController> m_controller;
};
