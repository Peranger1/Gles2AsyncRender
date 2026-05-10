#pragma once

#include "image_effect_types.h"

struct PhotoEditorSession
{
    void *handle = nullptr;

    ImageEffectParameters latestParameters;
    ImageEffectParameters processingParameters;

    bool hasLatestParameters = false;
    bool parametersDirty = false;

    int latestProgress = 0;
    bool processInFlight = false;
    bool renderReady = false;

    void reset();
};
