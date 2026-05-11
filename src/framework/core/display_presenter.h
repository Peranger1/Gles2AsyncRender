#pragma once

#include "async_render_types.h"

#include <QString>

class QOpenGLContext;
class QOpenGLFunctions;

class IDisplayHost
{
public:
    virtual ~IDisplayHost() = default;

    virtual QOpenGLContext *glContext() const = 0;
    virtual QOpenGLFunctions *glFunctions() const = 0;
    virtual QSize outputPixelSize() const = 0;
    virtual void requestUpdate() = 0;
};

class IFramePresenter
{
public:
    virtual ~IFramePresenter() = default;

    virtual bool initialize(IDisplayHost *host,
                            QString *error,
                            QString *runtimeLog) = 0;

    virtual void onOutputSizeChanged(const QSize &size) = 0;
    virtual void consume(const PublishedFrame &frame) = 0;
    virtual bool paint(QString *error, bool *releasedSlotForWorker) = 0;
    virtual void shutdown() = 0;
};
