#pragma once

#include <QMetaType>

struct ImageEffectParameters
{
    float brightness = 0.0f;
    float contrast = 1.0f;
    float zoom = 1.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    float rotationDegrees = 0.0f;
    int heavyGpuPassCount = 0;
    bool flipHorizontal = false;
    bool flipVertical = false;
};

Q_DECLARE_METATYPE(ImageEffectParameters)
