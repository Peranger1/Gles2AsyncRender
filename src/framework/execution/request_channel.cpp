#include "request_channel.h"

RequestChannel::RequestChannel(IRuntime *runtime,
                               std::unique_ptr<IRequestHandler> handler,
                               IRequestResultSink *resultSink)
    : m_runtime(runtime)
    , m_handler(std::move(handler))
    , m_resultSink(resultSink)
    , m_state(std::make_shared<State>())
{
    if (m_handler) {
        m_queuePolicy = createQueuePolicy(m_handler->descriptor());
    }
    m_state->self = this;
}

RequestChannel::~RequestChannel()
{
    shutdown();
}

QString RequestChannel::typeId() const
{
    return m_handler ? m_handler->descriptor().typeId : QString();
}

RequestTypeDescriptor RequestChannel::descriptor() const
{
    return m_handler ? m_handler->descriptor() : RequestTypeDescriptor();
}

std::unique_ptr<IRequestQueuePolicy> RequestChannel::createQueuePolicy(const RequestTypeDescriptor &descriptor)
{
    switch (descriptor.queuePolicy) {
    case RequestQueuePolicyKind::MergeWhileBusy:
        return std::make_unique<MergeWhileBusyPolicy>();
    case RequestQueuePolicyKind::SerialQueue:
    default:
        return std::make_unique<SerialQueuePolicy>();
    }
}

void RequestChannel::submit(const ExecutionRequest &request)
{
    if (m_shuttingDown || !m_handler || !m_queuePolicy || request.typeId != typeId()) {
        return;
    }

    if (m_hasActiveRequest) {
        QString error;
        if (!m_queuePolicy->submitWhileBusy(request, &error) && m_resultSink) {
            m_resultSink->onRequestFailed(request.requestId, error);
        }
        return;
    }

    if (!startRequest(request)) {
        drainWaitingQueue();
        return;
    }

    drainWaitingQueue();
}

void RequestChannel::shutdown()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;
    m_hasActiveRequest = false;
    m_activeRequest = {};
    m_activeExecution.reset();
    if (m_queuePolicy) {
        m_queuePolicy->clear();
    }
    if (m_handler) {
        m_handler->shutdown();
    }
    if (m_state) {
        m_state->self = nullptr;
        m_state.reset();
    }
}

IRuntime *RequestChannel::runtime() const
{
    return m_runtime;
}

bool RequestChannel::startRequest(const ExecutionRequest &request)
{
    ExecutionResult inlineResult;
    std::unique_ptr<IRequestExecution> asyncExecution;
    QString error;
    const StartDisposition disposition = m_handler->start(request,
                                                          *this,
                                                          &asyncExecution,
                                                          &inlineResult,
                                                          &error);
    switch (disposition) {
    case StartDisposition::CompletedInline:
        if (m_resultSink) {
            m_resultSink->onResultReady(inlineResult, *this);
        }
        return false;
    case StartDisposition::StartedAsync:
        m_hasActiveRequest = true;
        m_activeRequest = request;
        m_activeExecution = std::move(asyncExecution);
        if (m_activeExecution) {
            std::weak_ptr<State> weakState = m_state;
            m_activeExecution->setCompletionCallback([weakState]() {
                const std::shared_ptr<State> state = weakState.lock();
                if (state == nullptr || state->self == nullptr || state->self->m_shuttingDown) {
                    return;
                }
                state->self->onActiveExecutionFinished();
            });
        }
        return true;
    case StartDisposition::Failed:
    default:
        if (m_resultSink) {
            m_resultSink->onRequestFailed(request.requestId, error);
        }
        return false;
    }
}

void RequestChannel::onActiveExecutionFinished()
{
    if (m_shuttingDown || !m_hasActiveRequest || !m_activeExecution) {
        return;
    }

    ExecutionResult result;
    QString error;
    const bool ok = m_activeExecution->collectResult(&result, &error);
    const RequestId requestId = m_activeRequest.requestId;
    m_hasActiveRequest = false;
    m_activeRequest = {};
    m_activeExecution.reset();

    if (!ok) {
        if (m_resultSink) {
            m_resultSink->onRequestFailed(requestId, error);
        }
        drainWaitingQueue();
        return;
    }

    if (m_resultSink) {
        m_resultSink->onResultReady(result, *this);
    }
    drainWaitingQueue();
}

void RequestChannel::drainWaitingQueue()
{
    while (!m_shuttingDown && !m_hasActiveRequest && m_queuePolicy && m_queuePolicy->hasWaiting()) {
        const std::optional<ExecutionRequest> next = m_queuePolicy->takeNext();
        if (!next.has_value()) {
            break;
        }
        if (startRequest(*next)) {
            break;
        }
    }
}
