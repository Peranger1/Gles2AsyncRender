#include "render_backend_factory.h"

#include "platform_render_backend.h"

#include <QtGlobal>

#if defined(Q_OS_WIN)
#include "win_angle_d3d11/win_angle_render_backend.h"
#endif

std::unique_ptr<IPlatformRenderBackend> createDefaultRenderBackend()
{
#if defined(Q_OS_WIN)
    return std::make_unique<WinAngleRenderBackend>(3);
#else
    return nullptr;
#endif
}
