#pragma once

#include "async_render_types.h"
#include "render_runtime.h"

class QObject;

using AsyncRenderProgressCallback = void (*)(int progress, bool isEnd, void *userData);

class IAsyncRenderSession
{
public:
    virtual ~IAsyncRenderSession() = default;

    virtual bool initialize(IRenderRuntime &runtime, QString *error) = 0;
    virtual bool submitRequest(const AsyncRenderRequest &request,
                               QObject *callbackContext,
                               AsyncRenderProgressCallback progressCallback,
                               void *progressUserData,
                               QString *error) = 0;
    virtual void handleProgressEvent(int progress, bool isEnd) = 0;
    virtual bool isProcessInFlight() const noexcept = 0;
    virtual bool hasRenderReady() const noexcept = 0;
    virtual int latestProgress() const noexcept = 0;
    virtual quint64 requestSequence() const noexcept = 0;
    virtual bool renderReadyTexture(RenderedTexture *output, QString *error) = 0;
    virtual void discardRenderReady() = 0;
    virtual void shutdown() = 0;
};
