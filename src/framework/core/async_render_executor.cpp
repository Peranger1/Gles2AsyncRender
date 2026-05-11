#include "async_render_executor.h"

bool AsyncRenderExecutor::initialize(IRenderRuntime *runtime,
                                     IAsyncRenderSession *session,
                                     IFramePublisher *publisher,
                                     ISharedFrameSlotPool *slotPool,
                                     QString *error)
{
    if (runtime == nullptr || session == nullptr || publisher == nullptr || slotPool == nullptr) {
        if (error) {
            *error = QStringLiteral("AsyncRenderExecutor requires runtime, session, publisher, and slot pool.");
        }
        return false;
    }

    m_runtime = runtime;
    m_session = session;
    m_publisher = publisher;
    m_slotPool = slotPool;
    m_latestRequest = {};
    m_hasLatestRequest = false;
    m_waitingForSlot = false;
    return true;
}

void AsyncRenderExecutor::updateLatestRequest(const AsyncRenderRequest &request)
{
    m_latestRequest = request;
    m_hasLatestRequest = true;
}

bool AsyncRenderExecutor::hasPendingRequest() const noexcept
{
    return m_hasLatestRequest;
}

bool AsyncRenderExecutor::isWaitingForSlot() const noexcept
{
    return m_waitingForSlot;
}

quint64 AsyncRenderExecutor::currentRequestSequence() const noexcept
{
    return m_hasLatestRequest ? m_latestRequest.sequence : 0;
}

bool AsyncRenderExecutor::tryStartProcess(QObject *callbackContext,
                                          AsyncRenderProgressCallback progressCallback,
                                          void *progressUserData,
                                          QString *error)
{
    if (m_session == nullptr || !m_hasLatestRequest) {
        return false;
    }
    if (m_session->isProcessInFlight() || m_session->hasRenderReady()) {
        return false;
    }

    return m_session->submitRequest(m_latestRequest,
                                    callbackContext,
                                    progressCallback,
                                    progressUserData,
                                    error);
}

bool AsyncRenderExecutor::tryPublishReadyFrame(quint64 frameIndex, PublishedFrame *frame, QString *error)
{
    if (m_session == nullptr || m_slotPool == nullptr || m_publisher == nullptr || frame == nullptr) {
        if (error) {
            *error = QStringLiteral("AsyncRenderExecutor is not fully initialized.");
        }
        return false;
    }
    if (!m_session->hasRenderReady()) {
        if (error) {
            *error = QStringLiteral("No completed render result is ready for publish.");
        }
        return false;
    }

    int renderSlot = -1;
    if (!m_slotPool->tryAcquireRenderSlot(&renderSlot)) {
        m_waitingForSlot = true;
        if (error) {
            error->clear();
        }
        return false;
    }

    if (!m_runtime->makeCurrent(error)) {
        m_slotPool->abandonRenderSlot(renderSlot);
        return false;
    }

    RenderedTexture output;
    if (!m_session->renderReadyTexture(&output, error)) {
        m_runtime->doneCurrent(nullptr);
        m_slotPool->abandonRenderSlot(renderSlot);
        return false;
    }

    if (!m_publisher->publishToSlot(output.textureId, output.size, renderSlot, error)) {
        m_runtime->doneCurrent(nullptr);
        m_slotPool->abandonRenderSlot(renderSlot);
        return false;
    }

    m_runtime->doneCurrent(nullptr);

    if (!m_slotPool->submitRenderedFrame(renderSlot, frameIndex, frame)) {
        m_slotPool->abandonRenderSlot(renderSlot);
        if (error) {
            *error = QStringLiteral("Failed to submit the published frame into the shared slot pool.");
        }
        return false;
    }

    m_waitingForSlot = false;
    return true;
}

void AsyncRenderExecutor::shutdown()
{
    m_runtime = nullptr;
    m_session = nullptr;
    m_publisher = nullptr;
    m_slotPool = nullptr;
    m_latestRequest = {};
    m_hasLatestRequest = false;
    m_waitingForSlot = false;
}
