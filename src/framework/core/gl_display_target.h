#pragma once

#include "display_target.h"

class QOpenGLContext;
class QOpenGLFunctions;

class IGlDisplayTarget : public IDisplayTarget
{
public:
    ~IGlDisplayTarget() override = default;

    virtual QOpenGLContext *glContext() const = 0;
    virtual QOpenGLFunctions *glFunctions() const = 0;
};
