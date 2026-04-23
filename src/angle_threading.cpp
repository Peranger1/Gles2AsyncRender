#include "angle_threading.h"

#include <QGuiApplication>
#include <QOpenGLContext>
#include <QSurface>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>

#include <QtANGLE/EGL/egl.h>
#include <QtANGLE/EGL/eglext.h>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
using EglGetErrorFn = EGLint (EGLAPIENTRY *)(void);
using EglQueryStringFn = const char * (EGLAPIENTRY *)(EGLDisplay, EGLint);
using EglGetCurrentDisplayFn = EGLDisplay (EGLAPIENTRY *)(void);
using EglGetProcAddressFn = __eglMustCastToProperFunctionPointerType (EGLAPIENTRY *)(const char *);

struct ResolvedEglApi
{
    HMODULE module = nullptr;
    QString modulePath;
    EglGetErrorFn getError = nullptr;
    EglQueryStringFn queryString = nullptr;
    EglGetCurrentDisplayFn getCurrentDisplay = nullptr;
    EglGetProcAddressFn getProcAddress = nullptr;
    PFNEGLQUERYDISPLAYATTRIBEXTPROC queryDisplayAttrib = nullptr;
    PFNEGLQUERYDEVICEATTRIBEXTPROC queryDeviceAttrib = nullptr;

    bool isValid() const
    {
        return module && getError && queryString && getCurrentDisplay && getProcAddress;
    }
};

QString eglErrorToString(EGLint error)
{
    return QStringLiteral("0x%1").arg(static_cast<unsigned int>(error), 0, 16);
}

QString pointerToString(const void *value)
{
    return QStringLiteral("0x%1").arg(quintptr(value), QT_POINTER_SIZE * 2, 16, QLatin1Char('0'));
}

QString modulePathForHandle(HMODULE module)
{
    if (!module) {
        return QString();
    }

    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, buffer, MAX_PATH);
    return length > 0 ? QString::fromWCharArray(buffer, int(length)) : QString();
}

template <typename T>
T resolveModuleSymbol(HMODULE module, const char *name)
{
    return module ? reinterpret_cast<T>(::GetProcAddress(module, name)) : nullptr;
}

template <typename T>
T resolveEglSymbol(HMODULE module, EglGetProcAddressFn getProcAddress, const char *name)
{
    if (T proc = resolveModuleSymbol<T>(module, name)) {
        return proc;
    }

    return getProcAddress ? reinterpret_cast<T>(getProcAddress(name)) : nullptr;
}

HMODULE chooseQtEglModule(QPlatformNativeInterface *native, QStringList *probeLog)
{
    const auto logModule = [&](const char *label, HMODULE module) {
        if (!probeLog) {
            return;
        }

        const QString path = modulePathForHandle(module);
        probeLog->append(QStringLiteral("%1 -> %2%3")
                             .arg(QString::fromLatin1(label),
                                  pointerToString(module),
                                  path.isEmpty() ? QString() : QStringLiteral(" : %1").arg(path)));
    };

    HMODULE glesModule = nullptr;
    if (native) {
        glesModule = static_cast<HMODULE>(native->nativeResourceForIntegration(QByteArrayLiteral("glhandle")));
    }
    logModule("integration(glhandle)", glesModule);

    const HMODULE eglDebugModule = ::GetModuleHandleW(L"libEGLd.dll");
    const HMODULE eglReleaseModule = ::GetModuleHandleW(L"libEGL.dll");
    logModule("loaded(libEGLd.dll)", eglDebugModule);
    logModule("loaded(libEGL.dll)", eglReleaseModule);

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

ResolvedEglApi resolveQtEglApi(QOpenGLContext *context, QStringList *probeLog)
{
    ResolvedEglApi api;

    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    api.module = chooseQtEglModule(native, probeLog);
    api.modulePath = modulePathForHandle(api.module);

    if (!api.module) {
        if (probeLog) {
            probeLog->append(QStringLiteral("No loaded Qt EGL module could be identified."));
        }
        return api;
    }

    api.getError = resolveModuleSymbol<EglGetErrorFn>(api.module, "eglGetError");
    api.queryString = resolveModuleSymbol<EglQueryStringFn>(api.module, "eglQueryString");
    api.getCurrentDisplay = resolveModuleSymbol<EglGetCurrentDisplayFn>(api.module, "eglGetCurrentDisplay");
    api.getProcAddress = resolveModuleSymbol<EglGetProcAddressFn>(api.module, "eglGetProcAddress");

    if (api.getProcAddress) {
        api.queryDisplayAttrib =
            resolveEglSymbol<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(api.module, api.getProcAddress, "eglQueryDisplayAttribEXT");
        api.queryDeviceAttrib =
            resolveEglSymbol<PFNEGLQUERYDEVICEATTRIBEXTPROC>(api.module, api.getProcAddress, "eglQueryDeviceAttribEXT");
    }

    if (probeLog) {
        probeLog->append(QStringLiteral("Selected Qt EGL module -> %1%2")
                             .arg(pointerToString(api.module),
                                  api.modulePath.isEmpty() ? QString() : QStringLiteral(" : %1").arg(api.modulePath)));
        probeLog->append(QStringLiteral("EGL core symbols: eglGetError=%1 eglQueryString=%2 eglGetCurrentDisplay=%3 eglGetProcAddress=%4")
                             .arg(pointerToString(reinterpret_cast<void *>(api.getError)),
                                  pointerToString(reinterpret_cast<void *>(api.queryString)),
                                  pointerToString(reinterpret_cast<void *>(api.getCurrentDisplay)),
                                  pointerToString(reinterpret_cast<void *>(api.getProcAddress))));
        probeLog->append(QStringLiteral("EGL extension symbols: eglQueryDisplayAttribEXT=%1 eglQueryDeviceAttribEXT=%2")
                             .arg(pointerToString(reinterpret_cast<void *>(api.queryDisplayAttrib)),
                                  pointerToString(reinterpret_cast<void *>(api.queryDeviceAttrib))));
    }

    Q_UNUSED(context);
    return api;
}

bool isUsableEglDisplay(EGLDisplay display, const ResolvedEglApi &api, QString *details)
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

EGLDisplay queryQtNativeEglDisplay(QOpenGLContext *context, const ResolvedEglApi &api, QStringList *probeLog)
{
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    if (!native || !context) {
        return EGL_NO_DISPLAY;
    }

    const QByteArray displayResource = QByteArrayLiteral("egldisplay");
    const QByteArray contextResource = QByteArrayLiteral("eglcontext");
    const QByteArray configResource = QByteArrayLiteral("eglconfig");
    const QByteArray renderingContextResource = QByteArrayLiteral("renderingcontext");

    const auto appendHandleLog = [&](const QByteArray &resource, void *handle) {
        if (!probeLog) {
            return;
        }

        probeLog->append(QStringLiteral("context(%1) -> %2")
                             .arg(QString::fromLatin1(resource), pointerToString(handle)));
    };

    appendHandleLog(renderingContextResource, native->nativeResourceForContext(renderingContextResource, context));
    appendHandleLog(contextResource, native->nativeResourceForContext(contextResource, context));
    appendHandleLog(configResource, native->nativeResourceForContext(configResource, context));

    void *handle = native->nativeResourceForContext(displayResource, context);
    if (!handle) {
        if (probeLog) {
            probeLog->append(QStringLiteral("context(%1) -> null").arg(QString::fromLatin1(displayResource)));
        }
        return EGL_NO_DISPLAY;
    }

    const EGLDisplay display = static_cast<EGLDisplay>(handle);
    QString details;
    const bool ok = isUsableEglDisplay(display, api, &details);
    if (probeLog) {
        probeLog->append(QStringLiteral("context(%1) -> %2 : %3")
                             .arg(QString::fromLatin1(displayResource), pointerToString(handle), details));
    }
    return ok ? display : EGL_NO_DISPLAY;
}
} // namespace

AngleThreadingInfo ensureAngleD3D11MultithreadProtection()
{
    AngleThreadingInfo info;

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context) {
        info.message = QStringLiteral("ANGLE threading probe skipped: no current OpenGL context.");
        return info;
    }

    info.isAngleBackend = context->isOpenGLES() && QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES;
    if (!info.isAngleBackend) {
        info.multithreadProtected = true;
        info.message = QStringLiteral("Current backend is not ANGLE/LibGLES; no D3D11 threading patch is required.");
        return info;
    }

    QStringList probeLog;
    const ResolvedEglApi api = resolveQtEglApi(context, &probeLog);
    if (!api.isValid()) {
        info.message = QStringLiteral("ANGLE threading patch failed: could not resolve the Qt-loaded EGL API.\n%1")
                           .arg(probeLog.join(QStringLiteral("\n")));
        return info;
    }

    EGLDisplay display = queryQtNativeEglDisplay(context, api, &probeLog);
    if (display == EGL_NO_DISPLAY) {
        QString details;
        display = api.getCurrentDisplay();
        const bool currentOk = isUsableEglDisplay(display, api, &details);
        probeLog.append(QStringLiteral("eglGetCurrentDisplay() -> %1 : %2")
                            .arg(pointerToString(display), details));
        if (!currentOk) {
            display = EGL_NO_DISPLAY;
        }
    }
    if (display == EGL_NO_DISPLAY) {
        info.message = QStringLiteral("ANGLE threading patch failed: no valid EGLDisplay candidate was found.\n%1")
                           .arg(probeLog.join(QStringLiteral("\n")));
        return info;
    }

    if (!api.queryDisplayAttrib || !api.queryDeviceAttrib) {
        info.message = QStringLiteral("ANGLE threading patch failed: EGL_EXT_device_query entry points are unavailable.\n%1")
                           .arg(probeLog.join(QStringLiteral("\n")));
        return info;
    }

    EGLAttrib deviceAttrib = 0;
    if (!api.queryDisplayAttrib(display, EGL_DEVICE_EXT, &deviceAttrib) || deviceAttrib == 0) {
        info.message = QStringLiteral("ANGLE threading patch failed: eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed with EGL error %1.\nResolved EGLDisplay=%2\n%3")
                           .arg(eglErrorToString(api.getError()),
                                pointerToString(display),
                                probeLog.join(QStringLiteral("\n")));
        return info;
    }

    EGLAttrib d3d11DeviceAttrib = 0;
    if (!api.queryDeviceAttrib(reinterpret_cast<EGLDeviceEXT>(deviceAttrib), EGL_D3D11_DEVICE_ANGLE, &d3d11DeviceAttrib)
        || d3d11DeviceAttrib == 0) {
        info.message = QStringLiteral("ANGLE threading patch failed: EGL_D3D11_DEVICE_ANGLE is unavailable (EGL error %1).")
                           .arg(eglErrorToString(api.getError()));
        return info;
    }

    ID3D11Device *device = reinterpret_cast<ID3D11Device *>(d3d11DeviceAttrib);
    ComPtr<ID3D11DeviceContext> immediateContext;
    device->GetImmediateContext(&immediateContext);
    if (!immediateContext) {
        info.message = QStringLiteral("ANGLE threading patch failed: the D3D11 immediate context is null.");
        return info;
    }

    ComPtr<ID3D11Multithread> multithread;
    const HRESULT hr = immediateContext.As(&multithread);
    if (FAILED(hr) || !multithread) {
        info.message = QStringLiteral("ANGLE threading patch failed: ID3D11Multithread is unavailable (HRESULT 0x%1).")
                           .arg(static_cast<unsigned int>(hr), 0, 16);
        return info;
    }

    multithread->SetMultithreadProtected(TRUE);
    info.multithreadProtected = multithread->GetMultithreadProtected() == TRUE;
    info.message = info.multithreadProtected
        ? QStringLiteral("ANGLE threading patch applied: ID3D11Multithread protection is enabled.\nResolved EGLDisplay=%1\n%2")
              .arg(pointerToString(display), probeLog.join(QStringLiteral("\n")))
        : QStringLiteral("ANGLE threading patch attempted, but ID3D11Multithread protection is still disabled.\nResolved EGLDisplay=%1\n%2")
              .arg(pointerToString(display), probeLog.join(QStringLiteral("\n")));
    return info;
}
