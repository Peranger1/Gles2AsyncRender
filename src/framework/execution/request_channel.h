#pragma once

#include "merge_while_busy_policy.h"
#include "request_handler.h"
#include "request_queue_policy.h"
#include "request_result_sink.h"
#include "serial_queue_policy.h"

#include <memory>

class RequestChannel final : public IExecutionContext
{
public:
    RequestChannel(IRuntime *runtime,
                   std::unique_ptr<IRequestHandler> handler,
                   IRequestResultSink *resultSink);
    ~RequestChannel();

    QString typeId() const;
    RequestTypeDescriptor descriptor() const;

    void submit(const ExecutionRequest &request);
    void shutdown();

    IRuntime *runtime() const override;

private:
    struct State final
    {
        RequestChannel *self = nullptr;
    };

    static std::unique_ptr<IRequestQueuePolicy> createQueuePolicy(const RequestTypeDescriptor &descriptor);
    bool startRequest(const ExecutionRequest &request);
    void onActiveExecutionFinished();
    void drainWaitingQueue();

    IRuntime *m_runtime = nullptr;
    std::unique_ptr<IRequestHandler> m_handler;
    std::unique_ptr<IRequestQueuePolicy> m_queuePolicy;
    IRequestResultSink *m_resultSink = nullptr;
    bool m_hasActiveRequest = false;
    ExecutionRequest m_activeRequest;
    std::unique_ptr<IRequestExecution> m_activeExecution;
    bool m_shuttingDown = false;
    std::shared_ptr<State> m_state;
};
