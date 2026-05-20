#include "platform_backend_factory.h"

#include "platform_backend.h"

#if defined(Q_OS_WIN)
#include "win_angle_d3d11/win_angle_platform_backend.h"
#elif defined(Q_OS_MACOS)
#include "mac_cocoa_gl/mac_cocoa_gl_platform_backend.h"
#endif

#include <QtGlobal>

std::unique_ptr<IPlatformBackend> createDefaultPlatformBackend(int slotCount)
{
#if defined(Q_OS_WIN)
    return createPlatformBackend<WinAnglePlatformBackend>(slotCount);
#elif defined(Q_OS_MACOS)
    return createPlatformBackend<MacCocoaGlPlatformBackend>(slotCount);
#else
    Q_UNUSED(slotCount);
    return {};
#endif
}
