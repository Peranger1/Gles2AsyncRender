#pragma once

#include "framework/backend/platform_render_backend.h"

#include <memory>

class D3D11SharedSlotPool;

class WinAngleRenderBackend final : public IPlatformRenderBackend
{
public:
    explicit WinAngleRenderBackend(int slotCount = 3);

    IFrameWriter *frameWriter() const override;
    std::shared_ptr<IFrameReader> sharedFrameReader() const override;
    std::unique_ptr<IWorkRuntime> createRuntime() const override;
    std::unique_ptr<IFramePublisher> createPublisher() const override;
    std::unique_ptr<IFramePresenter> createPresenter() const override;

private:
    std::shared_ptr<D3D11SharedSlotPool> m_slotPool;
};
