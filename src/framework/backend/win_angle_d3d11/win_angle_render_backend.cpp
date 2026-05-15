#include "win_angle_render_backend.h"

#include "angle_standalone_runtime.h"
#include "d3d11_frame_publisher.h"
#include "d3d11_shared_slot_pool.h"
#include "framework/qt/qt_angle_display_presenter.h"

WinAngleRenderBackend::WinAngleRenderBackend(int slotCount)
    : m_slotPool(std::make_shared<D3D11SharedSlotPool>(slotCount))
{
}

IFrameWriter *WinAngleRenderBackend::frameWriter() const
{
    return m_slotPool.get();
}

std::shared_ptr<IFrameReader> WinAngleRenderBackend::sharedFrameReader() const
{
    return m_slotPool;
}

std::unique_ptr<IWorkRuntime> WinAngleRenderBackend::createRuntime() const
{
    return std::make_unique<AngleStandaloneRuntime>();
}

std::unique_ptr<IFramePublisher> WinAngleRenderBackend::createPublisher() const
{
    return std::make_unique<D3D11FramePublisher>();
}

std::unique_ptr<IFramePresenter> WinAngleRenderBackend::createPresenter() const
{
    return std::make_unique<QtAngleDisplayPresenter>();
}
