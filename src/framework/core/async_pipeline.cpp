#include "async_pipeline.h"

#include "work_scheduler.h"

bool initializeAsyncPipeline(IAsyncPipeline *pipeline,
                             IWorkRuntime *runtime,
                             IWorkProcessor *processor,
                             IArtifactPublisher *publisher,
                             IWorkScheduler *scheduler,
                             QString *error)
{
    return pipeline != nullptr
        && pipeline->initialize(runtime, processor, publisher, scheduler, error);
}
