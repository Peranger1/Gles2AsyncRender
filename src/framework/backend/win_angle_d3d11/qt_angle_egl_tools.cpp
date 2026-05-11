#include "qt_angle_egl_tools.h"

#include <QGuiApplication>
#include <QOpenGLFunctions>
#include <qpa/qplatformnativeinterface.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

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

QString adapterLuidString(ID3D11Device *device)
{
    if (device == nullptr) {
        return {};
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice) {
        return {};
    }

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter) {
        return {};
    }

    DXGI_ADAPTER_DESC desc = {};
    if (FAILED(adapter->GetDesc(&desc))) {
        return {};
    }

    return QStringLiteral("0x%1:0x%2")
        .arg(static_cast<quint32>(desc.AdapterLuid.HighPart), 8, 16, QLatin1Char('0'))
        .arg(desc.AdapterLuid.LowPart, 8, 16, QLatin1Char('0'));
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

RendererIdentity queryRendererIdentity(QOpenGLContext *context,
                                       EGLDisplay display,
                                       const ResolvedEglApi &api,
                                       QStringList *probeLog)
{
    RendererIdentity identity;
    identity.eglModule = api.module;
    identity.eglModulePath = api.modulePath;
    identity.eglDisplay = display;

    if (display != EGL_NO_DISPLAY && api.isValid()) {
        api.getError();
        const char *eglVendor = api.queryString(display, EGL_VENDOR);
        const EGLint eglVendorError = api.getError();
        if (eglVendor != nullptr && eglVendorError == EGL_SUCCESS) {
            identity.eglVendor = QString::fromLatin1(eglVendor);
        }

        api.getError();
        const char *eglVersion = api.queryString(display, EGL_VERSION);
        const EGLint eglVersionError = api.getError();
        if (eglVersion != nullptr && eglVersionError == EGL_SUCCESS) {
            identity.eglVersion = QString::fromLatin1(eglVersion);
        }

        if (api.queryDisplayAttrib != nullptr && api.queryDeviceAttrib != nullptr) {
            EGLAttrib deviceAttrib = 0;
            if (api.queryDisplayAttrib(display, EGL_DEVICE_EXT, &deviceAttrib) && deviceAttrib != 0) {
                identity.eglDevice = reinterpret_cast<EGLDeviceEXT>(deviceAttrib);
                if (probeLog) {
                    probeLog->append(QStringLiteral("eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) -> %1")
                                         .arg(pointerToString(reinterpret_cast<const void *>(identity.eglDevice))));
                }

                EGLAttrib d3d11DeviceAttrib = 0;
                if (api.queryDeviceAttrib(identity.eglDevice, EGL_D3D11_DEVICE_ANGLE, &d3d11DeviceAttrib)
                    && d3d11DeviceAttrib != 0) {
                    identity.d3d11Device = reinterpret_cast<ID3D11Device *>(d3d11DeviceAttrib);
                    identity.adapterLuid = adapterLuidString(identity.d3d11Device);
                    if (probeLog) {
                        probeLog->append(QStringLiteral("eglQueryDeviceAttribEXT(EGL_D3D11_DEVICE_ANGLE) -> %1 LUID=%2")
                                             .arg(pointerToString(identity.d3d11Device), identity.adapterLuid));
                    }
                }
            }
        }
    }

    if (context != nullptr && QOpenGLContext::currentContext() == context) {
        if (QOpenGLFunctions *functions = context->functions()) {
            const GLubyte *glVendor = functions->glGetString(GL_VENDOR);
            const GLubyte *glRenderer = functions->glGetString(GL_RENDERER);
            const GLubyte *glVersion = functions->glGetString(GL_VERSION);
            identity.glVendor = glVendor ? QString::fromLatin1(reinterpret_cast<const char *>(glVendor)) : QString();
            identity.glRenderer = glRenderer ? QString::fromLatin1(reinterpret_cast<const char *>(glRenderer)) : QString();
            identity.glVersion = glVersion ? QString::fromLatin1(reinterpret_cast<const char *>(glVersion)) : QString();
        }
    }

    return identity;
}
} // namespace QtAngleEglTools
