#include "photo_editor_session.h"

void PhotoEditorSession::reset()
{
    handle = nullptr;
    latestParameters = {};
    processingParameters = {};
    hasLatestParameters = false;
    parametersDirty = false;
    latestProgress = 0;
    processInFlight = false;
    renderReady = false;
}
