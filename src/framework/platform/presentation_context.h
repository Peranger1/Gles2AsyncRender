#pragma once

#include <QSurfaceFormat>

class QScreen;

struct PresentationContext final
{
    QScreen *screen = nullptr;
    QSurfaceFormat format;
};
