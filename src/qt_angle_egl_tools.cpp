#include "qt_angle_egl_tools.h"

#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>

#include <windows.h>

namespace QtAngleEglTools
{
namespace
{
template <typename T>
T resolveModuleSymbol(HMODULE module, const char *name)
{
    return module ? reinterpret_cast<T>(::GetProcAddress(module, name)) : nullptr;
}

template <typename T>
T resolveEglSymbol(HMODULE module,
                   __eglMustCastToProperFunctionPointerType (EGLAPIENTRY *getProcAddress)(const char *),
                   const char *name)
{
    if (T proc = resolveModuleSymbol<T>(module, name)) {
        return proc;
    }

    return getProcAddress ? reinterpret_cast<T>(getProcAddress(name)) : nullptr;
}

QString modulePathForHandle(HMODULE module)
{
    if (!module) {
        return {};
    }

    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(module, buffer, MAX_PATH);
    return length > 0 ? QString::fromWCharArray(buffer, int(length)) : QString();
}

HMODULE chooseQtEglModule(QPlatformNativeInterface *native, QStringList *probeLog)
{
    const auto appendModule = [&](const char *label, HMODULE module) {
        if (!probeLog) {
            return;
        }

        const QString path = modulePathForHandle(module);
        probeLog->append(QStringLiteral("%1 -> %2%3")
                             .arg(QString::fromLatin1(label),
                                  pointerToString(reinterpret_cast<const void *>(module)),
                                  path.isEmpty() ? QString() : QStringLiteral(" : %1").arg(path)));
    };

    HMODULE glesModule = nullptr;
    if (native) {
        glesModule = static_cast<HMODULE>(native->nativeResourceForIntegration(QByteArrayLiteral("glhandle")));
    }
    appendModule("integration(glhandle)", glesModule);

    const HMODULE eglDebugModule = ::GetModuleHandleW(L"libEGLd.dll");
    const HMODULE eglReleaseModule = ::GetModuleHandleW(L"libEGL.dll");
    appendModule("loaded(libEGLd.dll)", eglDebugModule);
    appendModule("loaded(libEGL.dll)", eglReleaseModule);

    const QString glesPath = modulePathForHandle(glesModule).toLower();
    if (glesPath.endsWith(QStringLiteral("libglesv2d.dll")) && eglDebugModule) {
        return eglDebugModule;
    }
    if (glesPath.endsWith(QStringLiteral("libglesv2.dll")) && eglReleaseModule) {
        return eglReleaseModule;
    }
    if (eglDebugModule) {
        return eglDebugModule;
    }
    return eglReleaseModule;
}
} // namespace

bool ResolvedEglApi::isValid() const
{
    return module != nullptr
        && getError != nullptr
        && queryString != nullptr
        && getCurrentDisplay != nullptr
        && getProcAddress != nullptr
        && chooseConfig != nullptr
        && getConfigAttrib != nullptr;
}

bool ResolvedEglApi::supportsD3DTextureImport() const
{
    return isValid()
        && createPbufferFromClientBuffer != nullptr
        && destroySurface != nullptr
        && bindTexImage != nullptr
        && releaseTexImage != nullptr
        && querySurfacePointerANGLE != nullptr;
}

QString eglErrorToString(EGLint error)
{
    return QStringLiteral("0x%1").arg(static_cast<unsigned int>(error), 0, 16);
}

QString pointerToString(const void *value)
{
    return QStringLiteral("0x%1").arg(quintptr(value), QT_POINTER_SIZE * 2, 16, QLatin1Char('0'));
}

QString pointerToString(quintptr value)
{
    return QStringLiteral("0x%1").arg(value, QT_POINTER_SIZE * 2, 16, QLatin1Char('0'));
}

ResolvedEglApi resolveEglApi(QOpenGLContext *context, QStringList *probeLog)
{
    Q_UNUSED(context);

    ResolvedEglApi api;
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    const HMODULE module = chooseQtEglModule(native, probeLog);
    api.module = module;
    api.modulePath = modulePathForHandle(module);

    if (!module) {
        if (probeLog) {
            probeLog->append(QStringLiteral("No loaded Qt EGL module could be identified."));
        }
        return api;
    }

    api.getError = resolveModuleSymbol<decltype(api.getError)>(module, "eglGetError");
    api.queryString = resolveModuleSymbol<decltype(api.queryString)>(module, "eglQueryString");
    api.getCurrentDisplay = resolveModuleSymbol<decltype(api.getCurrentDisplay)>(module, "eglGetCurrentDisplay");
    api.getProcAddress = resolveModuleSymbol<decltype(api.getProcAddress)>(module, "eglGetProcAddress");

    if (api.getProcAddress) {
        api.queryDisplayAttrib = resolveEglSymbol<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(module, api.getProcAddress, "eglQueryDisplayAttribEXT");
        api.queryDeviceAttrib = resolveEglSymbol<PFNEGLQUERYDEVICEATTRIBEXTPROC>(module, api.getProcAddress, "eglQueryDeviceAttribEXT");
        api.createPbufferFromClientBuffer =
            resolveEglSymbol<decltype(api.createPbufferFromClientBuffer)>(module, api.getProcAddress, "eglCreatePbufferFromClientBuffer");
        api.destroySurface = resolveEglSymbol<decltype(api.destroySurface)>(module, api.getProcAddress, "eglDestroySurface");
        api.bindTexImage = resolveEglSymbol<decltype(api.bindTexImage)>(module, api.getProcAddress, "eglBindTexImage");
        api.releaseTexImage = resolveEglSymbol<decltype(api.releaseTexImage)>(module, api.getProcAddress, "eglReleaseTexImage");
        api.chooseConfig = resolveEglSymbol<decltype(api.chooseConfig)>(module, api.getProcAddress, "eglChooseConfig");
        api.getConfigAttrib = resolveEglSymbol<decltype(api.getConfigAttrib)>(module, api.getProcAddress, "eglGetConfigAttrib");
        api.querySurfacePointerANGLE =
            resolveEglSymbol<PFNEGLQUERYSURFACEPOINTERANGLEPROC>(module, api.getProcAddress, "eglQuerySurfacePointerANGLE");
    }

    if (probeLog) {
        probeLog->append(QStringLiteral("Selected Qt EGL module -> %1%2")
                             .arg(pointerToString(reinterpret_cast<const void *>(module)),
                                  api.modulePath.isEmpty() ? QString() : QStringLiteral(" : %1").arg(api.modulePath)));
        probeLog->append(QStringLiteral(
            "EGL symbols: createPbufferFromClientBuffer=%1 bindTexImage=%2 releaseTexImage=%3 chooseConfig=%4 querySurfacePointerANGLE=%5")
                             .arg(pointerToString(reinterpret_cast<const void *>(api.createPbufferFromClientBuffer)),
                                  pointerToString(reinterpret_cast<const void *>(api.bindTexImage)),
                                  pointerToString(reinterpret_cast<const void *>(api.releaseTexImage)),
                                  pointerToString(reinterpret_cast<const void *>(api.chooseConfig)),
                                  pointerToString(reinterpret_cast<const void *>(api.querySurfacePointerANGLE))));
    }

    return api;
}

bool isUsableDisplay(EGLDisplay display, const ResolvedEglApi &api, QString *details)
{
    if (!api.isValid()) {
        if (details) {
            *details = QStringLiteral("EGL API resolution is incomplete.");
        }
        return false;
    }

    if (display == EGL_NO_DISPLAY || display == nullptr) {
        if (details) {
            *details = QStringLiteral("candidate is EGL_NO_DISPLAY");
        }
        return false;
    }

    api.getError();
    const char *version = api.queryString(display, EGL_VERSION);
    const EGLint versionError = api.getError();
    if (!version || versionError != EGL_SUCCESS) {
        if (details) {
            *details = QStringLiteral("eglQueryString(EGL_VERSION) failed with %1").arg(eglErrorToString(versionError));
        }
        return false;
    }

    api.getError();
    const char *vendor = api.queryString(display, EGL_VENDOR);
    const EGLint vendorError = api.getError();
    if (!vendor || vendorError != EGL_SUCCESS) {
        if (details) {
            *details = QStringLiteral("eglQueryString(EGL_VENDOR) failed with %1").arg(eglErrorToString(vendorError));
        }
        return false;
    }

    if (details) {
        *details = QStringLiteral("vendor=%1 version=%2")
                       .arg(QString::fromLatin1(vendor), QString::fromLatin1(version));
    }
    return true;
}

EGLDisplay queryDisplay(QOpenGLContext *context, const ResolvedEglApi &api, QStringList *probeLog)
{
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    if (!native || context == nullptr) {
        return EGL_NO_DISPLAY;
    }

    const QByteArray displayResource = QByteArrayLiteral("egldisplay");
    const QByteArray contextResource = QByteArrayLiteral("eglcontext");
    const QByteArray configResource = QByteArrayLiteral("eglconfig");
    const QByteArray renderingContextResource = QByteArrayLiteral("renderingcontext");

    const auto appendHandle = [&](const QByteArray &resource, void *handle) {
        if (!probeLog) {
            return;
        }

        probeLog->append(QStringLiteral("context(%1) -> %2")
                             .arg(QString::fromLatin1(resource), pointerToString(handle)));
    };

    appendHandle(renderingContextResource, native->nativeResourceForContext(renderingContextResource, context));
    appendHandle(contextResource, native->nativeResourceForContext(contextResource, context));
    appendHandle(configResource, native->nativeResourceForContext(configResource, context));

    void *handle = native->nativeResourceForContext(displayResource, context);
    if (!handle) {
        if (probeLog) {
            probeLog->append(QStringLiteral("context(%1) -> null").arg(QString::fromLatin1(displayResource)));
        }
        return EGL_NO_DISPLAY;
    }

    const EGLDisplay display = static_cast<EGLDisplay>(handle);
    QString details;
    const bool ok = isUsableDisplay(display, api, &details);
    if (probeLog) {
        probeLog->append(QStringLiteral("context(%1) -> %2 : %3")
                             .arg(QString::fromLatin1(displayResource), pointerToString(handle), details));
    }
    return ok ? display : EGL_NO_DISPLAY;
}

EGLConfig queryConfig(QOpenGLContext *context, QStringList *probeLog)
{
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    if (!native || context == nullptr) {
        return nullptr;
    }

    const QByteArray configResource = QByteArrayLiteral("eglconfig");
    void *handle = native->nativeResourceForContext(configResource, context);
    if (probeLog) {
        probeLog->append(QStringLiteral("context(%1) -> %2")
                             .arg(QString::fromLatin1(configResource), pointerToString(handle)));
    }
    return static_cast<EGLConfig>(handle);
}
} // namespace QtAngleEglTools
