#pragma once

#include "presentation_target.h"

class QOpenGLContext;
class QOpenGLFunctions;

class IGlPresentationTarget : public IPresentationTarget
{
public:
    ~IGlPresentationTarget() override = default;

    virtual QOpenGLContext *glContext() const = 0;
    virtual QOpenGLFunctions *glFunctions() const = 0;
};
