#include "async_render_worker.h"

#include "async_render_session.h"
#include "frame_publisher.h"
#include "render_runtime.h"
#include "shared_frame_slot_pool.h"
#include "runtime_diagnostics.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QMutexLocker>
#include <QTimer>

namespace
{
void logWorkerMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[AsyncRenderWorker]", message);
}

void logWorkerDiag(const QString &message)
{
    RuntimeDiagnostics::logDiag("[AsyncRenderWorker]", message);
}

void processProgressThunk(int progress, bool isEnd, void *userData)
{
    AsyncRenderWorker *worker = static_cast<AsyncRenderWorker *>(userData);
    if (worker == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(
        worker,
        "onProcessProgressEvent",
        Qt::QueuedConnection,
        Q_ARG(int, progress),
        Q_ARG(bool, isEnd));
}
}

AsyncRenderWorker::AsyncRenderWorker(QObject *parent)
    : QObject(parent)
{
}

AsyncRenderWorker::~AsyncRenderWorker()
{
    shutdown();
}

bool AsyncRenderWorker::configure(std::unique_ptr<IRenderRuntime> runtime,
                                  std::unique_ptr<IAsyncRenderSession> session,
                                  std::unique_ptr<IFramePublisher> publisher,
                                  QString *error)
{
    if (m_configured || !runtime || !session || !publisher) {
        if (error) {
            *error = QStringLiteral("AsyncRenderWorker requires runtime, session, and publisher exactly once.");
        }
        return false;
    }

    m_runtime = std::move(runtime);
    m_session = std::move(session);
    m_publisher = std::move(publisher);
    m_configured = true;
    return true;
}

bool AsyncRenderWorker::initialize(ISharedFrameSlotPool *slotPool)
{
    if (!m_configured || m_initialized || slotPool == nullptr) {
        return false;
    }

    m_slotPool = slotPool;

    QString error;
    if (!m_runtime->initialize(&error) || !m_session->initialize(*m_runtime, &error)) {
        emit workerFailed(error);
        m_slotPool = nullptr;
        return false;
    }

    if (!m_runtime->makeCurrent(&error)) {
        emit workerFailed(error);
        m_slotPool = nullptr;
        return false;
    }

    const bool publisherReady = m_publisher->initialize(m_runtime.get(), slotPool, &error);
    m_runtime->doneCurrent(nullptr);
    if (!publisherReady
        || !m_executor.initialize(m_runtime.get(), m_session.get(), m_publisher.get(), slotPool, &error)) {
        emit workerFailed(error);
        m_slotPool = nullptr;
        return false;
    }

    m_slotPool->reset();
    m_initialized = true;
    m_shuttingDown = false;
    m_waitingForFreeSlot = false;
    m_frameIndex = 0;
    logWorkerMessage(QStringLiteral("Initialized generic async render worker."));
    return true;
}

void AsyncRenderWorker::submitLatestRequest(const AsyncRenderRequest &request)
{
    if (!m_initialized || m_shuttingDown) {
        return;
    }

    m_executor.updateLatestRequest(request);
    schedulePump(0);
}

void AsyncRenderWorker::requestRender()
{
    pumpRender();
}

void AsyncRenderWorker::onSlotAvailable()
{
    if (!m_initialized || m_shuttingDown || !m_waitingForFreeSlot) {
        return;
    }

    m_waitingForFreeSlot = false;
    schedulePump(0);
}

void AsyncRenderWorker::shutdown()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;
    m_initialized = false;
    m_renderScheduled = false;
    m_waitingForFreeSlot = false;
    m_frameIndex = 0;

    m_executor.shutdown();

    if (m_session) {
        m_session->shutdown();
    }

    if (m_runtime) {
        if (m_runtime->makeCurrent(nullptr) && m_publisher) {
            m_publisher->releaseGlResources();
            m_runtime->doneCurrent(nullptr);
        }
        m_runtime->shutdown();
    }

    m_slotPool = nullptr;
}

void AsyncRenderWorker::onProcessProgressEvent(int progress, bool isEnd)
{
    if (!m_initialized || m_shuttingDown || !m_session) {
        return;
    }

    m_session->handleProgressEvent(progress, isEnd);
    emit processingProgressChanged(progress);

    if (isEnd) {
        schedulePump(0);
    }
}

void AsyncRenderWorker::schedulePump(int delayMs)
{
    QMutexLocker locker(&m_stateMutex);
    if (m_renderScheduled || m_shuttingDown) {
        return;
    }

    m_renderScheduled = true;
    QTimer::singleShot(qMax(0, delayMs), this, &AsyncRenderWorker::pumpRender);
}

void AsyncRenderWorker::pumpRender()
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_renderScheduled = false;
    }

    if (m_shuttingDown) {
        logWorkerDiag(QStringLiteral("[diag] pumpRender skipped because shutdown is in progress"));
        return;
    }

    if (!m_initialized || m_slotPool == nullptr || !m_session) {
        return;
    }

    QString error;
    if (m_session->hasRenderReady()) {
        QElapsedTimer timer;
        timer.start();
        PublishedFrame frame;
        if (!m_executor.tryPublishReadyFrame(m_frameIndex, &frame, &error)) {
            if (error.isEmpty()) {
                m_waitingForFreeSlot = m_executor.isWaitingForSlot();
                return;
            }
            emit workerFailed(error);
            return;
        }

        m_waitingForFreeSlot = false;
        logWorkerMessage(QStringLiteral("Published frame=%1 slot=%2 output=%3x%4 elapsedMs=%5")
                             .arg(m_frameIndex)
                             .arg(frame.slotIndex)
                             .arg(frame.size.width())
                             .arg(frame.size.height())
                             .arg(QString::number(double(timer.nsecsElapsed()) / 1000000.0, 'f', 2)));
        emit frameReady(frame.slotIndex, frame.generation, frame.size, frame.frameIndex);
        ++m_frameIndex;

        if (m_executor.hasPendingRequest()
            && m_executor.currentRequestSequence() > m_session->requestSequence()) {
            schedulePump(0);
        }
        return;
    }

    if (m_session->isProcessInFlight()) {
        return;
    }

    if (!m_executor.tryStartProcess(this, &processProgressThunk, this, &error) && !error.isEmpty()) {
        emit workerFailed(error);
    }
}
