#pragma once

#include "framework/core/frame_presenter.h"
#include "framework/core/frame_publisher.h"
#include "framework/core/shared_frame_slot_pool.h"
#include "framework/core/work_runtime.h"

#include <memory>

class IPlatformRenderBackend
{
public:
    virtual ~IPlatformRenderBackend() = default;

    virtual IFrameWriter *frameWriter() const = 0;
    virtual std::shared_ptr<IFrameReader> sharedFrameReader() const = 0;
    virtual std::unique_ptr<IWorkRuntime> createRuntime() const = 0;
    virtual std::unique_ptr<IFramePublisher> createPublisher() const = 0;
    virtual std::unique_ptr<IFramePresenter> createPresenter() const = 0;
};
