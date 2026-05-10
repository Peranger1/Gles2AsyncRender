#include "photo_editor_library_host.h"

#include "photo_editor_gles2_simulator.h"

#include <mutex>

namespace
{
std::once_flag g_photoEditorInitOnce;
bool g_photoEditorInitSucceeded = false;
QString g_photoEditorInitError;
} // namespace

bool PhotoEditorLibraryHost::initializeOnce(QOpenGLContext *libraryContext, QString *error)
{
    if (libraryContext == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorLibraryHost requires a valid library context.");
        }
        return false;
    }

    if (QOpenGLContext::currentContext() != libraryContext) {
        if (error) {
            *error = QStringLiteral("photo_editor_init must run with the library context current.");
        }
        return false;
    }

    std::call_once(g_photoEditorInitOnce, [error]() {
        QString initError;
        g_photoEditorInitSucceeded = photo_editor_init(&PhotoEditorLibraryHost::resolveGlProc, &initError);
        if (!g_photoEditorInitSucceeded) {
            g_photoEditorInitError = initError;
        }
        Q_UNUSED(error);
    });

    if (!g_photoEditorInitSucceeded) {
        if (error) {
            *error = g_photoEditorInitError.isEmpty()
                ? QStringLiteral("photo_editor_init failed.")
                : g_photoEditorInitError;
        }
        return false;
    }

    return true;
}

void *PhotoEditorLibraryHost::resolveGlProc(const char *name)
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr || name == nullptr) {
        return nullptr;
    }

    return reinterpret_cast<void *>(context->getProcAddress(name));
}
