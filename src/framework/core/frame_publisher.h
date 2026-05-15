#pragma once

#include "shared_frame_slot_pool.h"
#include "work_runtime.h"
#include "work_types.h"

#include <QString>

class IFramePublisher
{
public:
    virtual ~IFramePublisher() = default;

    virtual bool initialize(IWorkRuntime &runtime,
                            IFrameWriter &frameWriter,
                            QString *error) = 0;
    virtual bool publish(const WorkEnvelope &work,
                         const GpuTextureResult &gpuResult,
                         FrameTicket *ticket,
                         QString *error) = 0;
    virtual void shutdown() = 0;
};
