#pragma once

#include <QPointer>
#include <QSurfaceFormat>

#include <memory>

class QOffscreenSurface;
class QScreen;
class MacIoSurfaceTextureSlots;

struct MacCocoaGlSharedState final
{
    QPointer<QScreen> screen;
    QSurfaceFormat format;
    std::unique_ptr<QOffscreenSurface> offscreenSurface;
    std::shared_ptr<MacIoSurfaceTextureSlots> slotPool;
};
