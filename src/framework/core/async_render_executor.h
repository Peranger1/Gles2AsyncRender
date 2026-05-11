#pragma once

#include "frame_publisher.h"
#include "shared_frame_slot_pool.h"
#include "async_render_session.h"
#include "async_render_types.h"

#include <QString>

class QObject;
class IRenderRuntime;

class AsyncRenderExecutor final
{
public:
    bool initialize(IRenderRuntime *runtime,
                    IAsyncRenderSession *session,
                    IFramePublisher *publisher,
                    ISharedFrameSlotPool *slotPool,
                    QString *error);

    void updateLatestRequest(const AsyncRenderRequest &request);
    bool hasPendingRequest() const noexcept;
    bool isWaitingForSlot() const noexcept;
    quint64 currentRequestSequence() const noexcept;

    bool tryStartProcess(QObject *callbackContext,
                         AsyncRenderProgressCallback progressCallback,
                         void *progressUserData,
                         QString *error);

    bool tryPublishReadyFrame(quint64 frameIndex, PublishedFrame *frame, QString *error);

    void shutdown();

private:
    IRenderRuntime *m_runtime = nullptr;
    IAsyncRenderSession *m_session = nullptr;
    IFramePublisher *m_publisher = nullptr;
    ISharedFrameSlotPool *m_slotPool = nullptr;

    AsyncRenderRequest m_latestRequest;
    bool m_hasLatestRequest = false;
    bool m_waitingForSlot = false;
};
