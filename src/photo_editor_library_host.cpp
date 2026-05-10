#include "photo_editor_library_host.h"

#include "angle_standalone_runtime.h"
#include "photo_editor_gles2_simulator.h"

#include <mutex>

namespace
{
std::once_flag g_photoEditorInitOnce;
bool g_photoEditorInitSucceeded = false;
QString g_photoEditorInitError;
AngleStandaloneRuntime *g_photoEditorRuntime = nullptr;
} // namespace

bool PhotoEditorLibraryHost::initializeOnce(AngleStandaloneRuntime *runtime, QString *error)
{
    if (runtime == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorLibraryHost requires a valid standalone runtime.");
        }
        return false;
    }

    if (!runtime->procTable().isValid()) {
        if (error) {
            *error = QStringLiteral("PhotoEditorLibraryHost requires an initialized standalone proc table.");
        }
        return false;
    }

    if (g_photoEditorRuntime != nullptr && g_photoEditorRuntime != runtime) {
        if (error) {
            *error = QStringLiteral("photo_editor_init is already bound to a different standalone runtime.");
        }
        return false;
    }

    std::call_once(g_photoEditorInitOnce, [runtime, error]() {
        QString initError;
        g_photoEditorRuntime = runtime;
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
    if (g_photoEditorRuntime == nullptr || name == nullptr) {
        return nullptr;
    }

    return g_photoEditorRuntime->resolveProc(name);
}
