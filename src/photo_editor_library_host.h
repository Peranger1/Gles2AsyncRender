#pragma once

#include <QString>

class AngleStandaloneRuntime;

class PhotoEditorLibraryHost
{
public:
    bool initializeOnce(AngleStandaloneRuntime *runtime, QString *error);

private:
    static void *resolveGlProc(const char *name);
};
