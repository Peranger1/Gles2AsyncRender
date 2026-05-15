#include "async_task_facade.h"

#include "framework/core/async_job_controller.h"
#include "framework/core/serial_conflated_async_pipeline.h"
#include "framework/core/serial_conflated_work_scheduler.h"

AsyncTaskFacade::AsyncTaskFacade(QObject *parent)
    : QObject(parent)
    , m_controller(std::make_unique<AsyncJobController>())
{
    connect(m_controller.get(), &AsyncJobController::stateChanged,
            this, &AsyncTaskFacade::stateChanged);
    connect(m_controller.get(), &AsyncJobController::progressChanged,
            this, &AsyncTaskFacade::progressChanged);
    connect(m_controller.get(), &AsyncJobController::messageEmitted,
            this, &AsyncTaskFacade::messageEmitted);
    connect(m_controller.get(), &AsyncJobController::resultReady,
            this, &AsyncTaskFacade::resultReady);
    connect(m_controller.get(), &AsyncJobController::fatalError,
            this, &AsyncTaskFacade::fatalError);
}

AsyncTaskFacade::~AsyncTaskFacade()
{
    shutdown();
}

bool AsyncTaskFacade::initialize(IPlatformRenderBackend *backend,
                                 std::unique_ptr<IWorkProcessor> processor,
                                 QSize outputSize,
                                 QString *error,
                                 std::shared_ptr<IRequestCoalescer> coalescer)
{
    if (!m_controller) {
        if (error) {
            *error = QStringLiteral("AsyncTaskFacade controller is not available.");
        }
        return false;
    }

    return m_controller->initialize(backend,
                                    std::move(processor),
                                    std::make_unique<SerialConflatedWorkScheduler>(std::move(coalescer)),
                                    std::make_unique<SerialConflatedAsyncPipeline>(),
                                    outputSize,
                                    error);
}

bool AsyncTaskFacade::initializeWithoutBackend(std::unique_ptr<IWorkRuntime> runtime,
                                               std::unique_ptr<IWorkProcessor> processor,
                                               QString *error,
                                               std::shared_ptr<IRequestCoalescer> coalescer)
{
    if (!m_controller) {
        if (error) {
            *error = QStringLiteral("AsyncTaskFacade controller is not available.");
        }
        return false;
    }

    return m_controller->initializeWithoutBackend(std::move(runtime),
                                                  std::move(processor),
                                                  std::make_unique<SerialConflatedWorkScheduler>(std::move(coalescer)),
                                                  std::make_unique<SerialConflatedAsyncPipeline>(),
                                                  error);
}

bool AsyncTaskFacade::isInitialized() const noexcept
{
    return m_controller && m_controller->isInitialized();
}

QSize AsyncTaskFacade::outputSize() const noexcept
{
    return m_controller ? m_controller->outputSize() : QSize();
}

void AsyncTaskFacade::setOutputSize(QSize size)
{
    if (m_controller) {
        m_controller->setOutputSize(size);
    }
}

void AsyncTaskFacade::submit(const WorkEnvelope &work)
{
    if (m_controller) {
        m_controller->submit(work);
    }
}

void AsyncTaskFacade::requestPump()
{
    if (m_controller) {
        m_controller->requestPump();
    }
}

void AsyncTaskFacade::onPublicationCapacityAvailable()
{
    if (m_controller) {
        m_controller->onPublicationCapacityAvailable();
    }
}

void AsyncTaskFacade::shutdown()
{
    if (m_controller) {
        m_controller->shutdown();
    }
}
