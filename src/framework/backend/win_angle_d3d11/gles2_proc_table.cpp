#include "gles2_proc_table.h"

namespace
{
template <typename T>
T resolveProcAddress(const Gles2ProcTable::ResolveProc &resolver, const char *name)
{
    return resolver ? reinterpret_cast<T>(resolver(name)) : nullptr;
}
}

bool Gles2ProcTable::load(const ResolveProc &resolver, QString *error)
{
    if (!resolver) {
        if (error) {
            *error = QStringLiteral("A GLES2/EGL proc resolver is required.");
        }
        return false;
    }

#define GLES2_LOAD_PROC(name, type) \
    name = resolveProcAddress<name##Proc>(resolver, #name); \
    if (name == nullptr) { \
        if (error) { \
            *error = QStringLiteral("Failed to resolve required GLES2/EGL symbol: %1").arg(QStringLiteral(#name)); \
        } \
        return false; \
    }
    GLES2_PROC_TABLE_REQUIRED_EGL_SYMBOLS(GLES2_LOAD_PROC)
    GLES2_PROC_TABLE_GL_SYMBOLS(GLES2_LOAD_PROC)
#undef GLES2_LOAD_PROC

#define GLES2_LOAD_OPTIONAL_PROC(name, type) \
    name = resolveProcAddress<name##Proc>(resolver, #name);
    GLES2_PROC_TABLE_OPTIONAL_EGL_SYMBOLS(GLES2_LOAD_OPTIONAL_PROC)
#undef GLES2_LOAD_OPTIONAL_PROC

    return true;
}

bool Gles2ProcTable::isValid() const
{
#define GLES2_CHECK_PROC(name, type) && name != nullptr
    return true
        GLES2_PROC_TABLE_REQUIRED_EGL_SYMBOLS(GLES2_CHECK_PROC)
        GLES2_PROC_TABLE_GL_SYMBOLS(GLES2_CHECK_PROC);
#undef GLES2_CHECK_PROC
}

bool Gles2ProcTable::supportsAngleD3DTextureImport() const
{
    return eglCreatePbufferFromClientBuffer != nullptr
        && eglBindTexImage != nullptr
        && eglReleaseTexImage != nullptr
        && eglQuerySurfacePointerANGLE != nullptr;
}
