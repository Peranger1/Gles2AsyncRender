#include "async_job_controller.h"

#include "framework/backend/platform_render_backend.h"

namespace
{
QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}
}

AsyncJobController::AsyncJobController(QObject *parent)
    : QObject(parent)
{
}

AsyncJobController::~AsyncJobController()
{
    shutdown();
}

bool AsyncJobController::initialize(IPlatformRenderBackend *backend,
                                    std::unique_ptr<IWorkProcessor> processor,
                                    std::unique_ptr<IWorkScheduler> scheduler,
                                    std::unique_ptr<IAsyncPipeline> pipeline,
                                    QSize outputSize,
                                    QString *error)
{
    if (backend == nullptr) {
        if (error) {
            *error = QStringLiteral("AsyncJobController requires a valid platform render backend.");
        }
        return false;
    }

    return initializeInternal(backend->createRuntime(),
                              std::move(processor),
                              backend->createPublisher(),
                              backend->frameWriter(),
                              std::move(scheduler),
                              std::move(pipeline),
                              outputSize,
                              error);
}

bool AsyncJobController::initializeWithoutBackend(std::unique_ptr<IWorkRuntime> runtime,
                                                  std::unique_ptr<IWorkProcessor> processor,
                                                  std::unique_ptr<IWorkScheduler> scheduler,
                                                  std::unique_ptr<IAsyncPipeline> pipeline,
                                                  QString *error)
{
    return initializeInternal(std::move(runtime),
                              std::move(processor),
                              nullptr,
                              nullptr,
                              std::move(scheduler),
                              std::move(pipeline),
                              QSize(1, 1),
                              error);
}

bool AsyncJobController::initializeInternal(std::unique_ptr<IWorkRuntime> runtime,
                                            std::unique_ptr<IWorkProcessor> processor,
                                            std::unique_ptr<IFramePublisher> publisher,
                                            IFrameWriter *frameWriter,
                                            std::unique_ptr<IWorkScheduler> scheduler,
                                            std::unique_ptr<IAsyncPipeline> pipeline,
                                            QSize outputSize,
                                            QString *error)
{
    if (m_initialized || m_shuttingDown) {
        if (error) {
            *error = QStringLiteral("AsyncJobController is already initialized or shutting down.");
        }
        return false;
    }
    if (!runtime || !processor || !scheduler || !pipeline) {
        if (error) {
            *error = QStringLiteral("AsyncJobController requires runtime, processor, scheduler, and pipeline.");
        }
        return false;
    }

    m_runtime = std::move(runtime);
    m_processor = std::move(processor);
    m_publisher = std::move(publisher);
    m_scheduler = std::move(scheduler);
    m_pipeline = std::move(pipeline);
    m_frameWriter = frameWriter;
    m_outputSize = sanitizedSize(outputSize);
    m_shuttingDown = false;

    m_pipeline->setStateChangedCallback([this](RequestId requestId, WorkState state) {
        if (!m_shuttingDown) {
            emit stateChanged(requestId, state);
        }
    });
    m_pipeline->setProgressCallback([this](RequestId requestId, int progress, bool isFinal) {
        if (!m_shuttingDown) {
            emit progressChanged(requestId, progress, isFinal);
        }
    });
    m_pipeline->setMessageCallback([this](RequestId requestId, const QString &message) {
        if (!m_shuttingDown) {
            emit messageEmitted(requestId, message);
        }
    });
    m_pipeline->setResultReadyCallback([this](const JobResult &result) {
        if (!m_shuttingDown) {
            emit resultReady(result);
        }
    });
    m_pipeline->setErrorCallback([this](const QString &reason) {
        if (!m_shuttingDown && !reason.isEmpty()) {
            emit fatalError(reason);
        }
    });

    if (!m_pipeline->initialize(m_runtime.get(),
                                m_processor.get(),
                                m_publisher.get(),
                                m_frameWriter,
                                m_scheduler.get(),
                                error)) {
        shutdown();
        return false;
    }

    if (m_frameWriter != nullptr) {
        m_frameWriter->reset();
    }

    m_initialized = true;
    return true;
}

bool AsyncJobController::isInitialized() const noexcept
{
    return m_initialized;
}

QSize AsyncJobController::outputSize() const noexcept
{
    return m_outputSize;
}

void AsyncJobController::setOutputSize(QSize size)
{
    m_outputSize = sanitizedSize(size);
}

void AsyncJobController::submit(const WorkEnvelope &work)
{
    if (!m_initialized || m_shuttingDown || !m_pipeline) {
        return;
    }

    m_pipeline->submit(work);
}

void AsyncJobController::requestPump()
{
    if (!m_initialized || m_shuttingDown || !m_pipeline) {
        return;
    }

    m_pipeline->pump();
}

void AsyncJobController::onPublicationCapacityAvailable()
{
    if (!m_initialized || m_shuttingDown || !m_pipeline) {
        return;
    }

    m_pipeline->onPublicationCapacityAvailable();
}

void AsyncJobController::shutdown()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;
    m_initialized = false;

    if (m_pipeline) {
        m_pipeline->shutdown();
    }

    m_pipeline.reset();
    m_scheduler.reset();
    m_publisher.reset();
    m_processor.reset();
    m_runtime.reset();
    m_frameWriter = nullptr;
    m_outputSize = QSize();
}
