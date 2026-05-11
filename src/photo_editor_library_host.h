#pragma once

#include "framework/core/render_runtime.h"

#include <QString>

class PhotoEditorLibraryHost
{
public:
    bool initializeOnce(IRenderRuntime *runtime, QString *error);

private:
    static void *resolveGlProc(const char *name);
};
