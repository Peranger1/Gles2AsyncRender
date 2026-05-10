#pragma once

#include <QOpenGLContext>
#include <QString>

class PhotoEditorLibraryHost
{
public:
    bool initializeOnce(QOpenGLContext *libraryContext, QString *error);

private:
    static void *resolveGlProc(const char *name);
};
