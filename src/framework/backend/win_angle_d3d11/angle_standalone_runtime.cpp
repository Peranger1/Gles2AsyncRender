#include "angle_standalone_runtime.h"
#include "runtime_diagnostics.h"

#include <QByteArray>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
QString pointerToString(const void *value)
{
    return QStringLiteral("0x%1").arg(quintptr(value), QT_POINTER_SIZE * 2, 16, QLatin1Char('0'));
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

QString hresultToString(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<unsigned int>(hr), 0, 16);
}

QString eglErrorToString(EGLint error)
{
    return QStringLiteral("0x%1").arg(static_cast<unsigned int>(error), 0, 16);
}

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

bool createD3D11Device(ComPtr<ID3D11Device> *device,
                       ComPtr<ID3D11DeviceContext> *deviceContext,
                       QString *error)
{
    if (device == nullptr || deviceContext == nullptr) {
        if (error) {
            *error = QStringLiteral("The standalone runtime D3D11 device outputs are invalid.");
        }
        return false;
    }

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr,
                                   flags,
                                   kLevels,
                                   ARRAYSIZE(kLevels),
                                   D3D11_SDK_VERSION,
                                   device->ReleaseAndGetAddressOf(),
                                   &featureLevel,
                                   deviceContext->ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr,
                               D3D_DRIVER_TYPE_WARP,
                               nullptr,
                               flags,
                               kLevels,
                               ARRAYSIZE(kLevels),
                               D3D11_SDK_VERSION,
                               device->ReleaseAndGetAddressOf(),
                               &featureLevel,
                               deviceContext->ReleaseAndGetAddressOf());
    }

    if (FAILED(hr) || !(*device) || !(*deviceContext)) {
        if (error) {
            *error = QStringLiteral("The standalone runtime D3D11 device creation failed: %1")
                         .arg(hresultToString(hr));
        }
        return false;
    }

    Q_UNUSED(featureLevel);
    return true;
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

void logStandaloneMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[AngleStandaloneRuntime]", message);
}

void logStandaloneDiag(const QString &message)
{
    RuntimeDiagnostics::logDiag("[AngleStandaloneRuntime]", message);
}

QString eglResultString(EGLBoolean ok, HMODULE eglModule)
{
    const auto eglGetErrorFn = resolveModuleSymbol<EGLint (EGLAPIENTRY *)(void)>(eglModule, "eglGetError");
    if (ok == EGL_TRUE) {
        return QStringLiteral("ok");
    }
    return QStringLiteral("failed error=%1").arg(eglErrorToString(eglGetErrorFn ? eglGetErrorFn() : EGL_SUCCESS));
}
}

struct AngleStandaloneRuntime::Impl final
{
    HMODULE eglModule = nullptr;
    HMODULE glesModule = nullptr;
    QString eglModulePath;
    QString glesModulePath;

    __eglMustCastToProperFunctionPointerType (EGLAPIENTRY *eglGetProcAddressFn)(const char *) = nullptr;
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT = nullptr;
    PFNEGLCREATEDEVICEANGLEPROC eglCreateDeviceANGLE = nullptr;
    PFNEGLRELEASEDEVICEANGLEPROC eglReleaseDeviceANGLE = nullptr;
    PFNEGLQUERYDISPLAYATTRIBEXTPROC eglQueryDisplayAttribEXT = nullptr;
    PFNEGLQUERYDEVICEATTRIBEXTPROC eglQueryDeviceAttribEXT = nullptr;
    EGLBoolean (EGLAPIENTRY *eglInitializeFn)(EGLDisplay, EGLint *, EGLint *) = nullptr;
    EGLBoolean (EGLAPIENTRY *eglChooseConfigFn)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = nullptr;
    EGLContext (EGLAPIENTRY *eglCreateContextFn)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = nullptr;
    EGLSurface (EGLAPIENTRY *eglCreatePbufferSurfaceFn)(EGLDisplay, EGLConfig, const EGLint *) = nullptr;
    EGLBoolean (EGLAPIENTRY *eglMakeCurrentFn)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    EGLBoolean (EGLAPIENTRY *eglDestroyContextFn)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (EGLAPIENTRY *eglDestroySurfaceFn)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (EGLAPIENTRY *eglTerminateFn)(EGLDisplay) = nullptr;

    Gles2ProcTable procTable;
    ComPtr<ID3D11Device> d3d11Device;
    ComPtr<ID3D11DeviceContext> d3d11DeviceContext;
    EGLDeviceEXT eglDevice = EGL_NO_DEVICE_EXT;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig eglConfig = nullptr;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    bool initialized = false;
    QString adapterLuid;
    QString glVendor;
    QString glRenderer;
    QString glVersion;

    bool initialize(QString *error)
    {
        if (initialized) {
            return true;
        }

        if (!resolveModules(error) || !resolveEglFunctions(error) || !createStandaloneContext(error)) {
            shutdown();
            return false;
        }

        if (!makeCurrent(error)) {
            shutdown();
            return false;
        }

        const auto resolver = [this](const char *name) { return resolveProc(name); };
        if (!procTable.load(resolver, error)) {
            doneCurrent(nullptr);
            shutdown();
            return false;
        }

        const GLubyte *vendor = procTable.glGetString(GL_VENDOR);
        const GLubyte *renderer = procTable.glGetString(GL_RENDERER);
        const GLubyte *version = procTable.glGetString(GL_VERSION);
        adapterLuid = adapterLuidString(d3d11Device.Get());
        glVendor = vendor ? QString::fromLatin1(reinterpret_cast<const char *>(vendor)) : QString();
        glRenderer = renderer ? QString::fromLatin1(reinterpret_cast<const char *>(renderer)) : QString();
        glVersion = version ? QString::fromLatin1(reinterpret_cast<const char *>(version)) : QString();
        logStandaloneMessage(QStringLiteral("Initialized standalone ANGLE runtime. eglDisplay=%1 d3d11Device=%2 adapterLuid=%3 vendor=%4 renderer=%5 version=%6")
                                 .arg(pointerToString(eglDisplay))
                                 .arg(pointerToString(d3d11Device.Get()))
                                 .arg(adapterLuid)
                                 .arg(glVendor)
                                 .arg(glRenderer)
                                 .arg(glVersion));
        doneCurrent(nullptr);
        initialized = true;
        return true;
    }

    bool makeCurrent(QString *error)
    {
        if (eglDisplay == EGL_NO_DISPLAY || eglContext == EGL_NO_CONTEXT || eglSurface == EGL_NO_SURFACE) {
            if (error) {
                *error = QStringLiteral("The standalone ANGLE runtime is not fully initialized.");
            }
            return false;
        }

        if (eglMakeCurrentFn(eglDisplay, eglSurface, eglSurface, eglContext) != EGL_TRUE) {
            if (error) {
                *error = QStringLiteral("eglMakeCurrent failed for the standalone runtime: %1")
                             .arg(eglErrorToString(resolveModuleSymbol<EGLint (EGLAPIENTRY *)(void)>(eglModule, "eglGetError")()));
            }
            return false;
        }

        return true;
    }

    bool doneCurrent(QString *error)
    {
        if (eglDisplay == EGL_NO_DISPLAY || eglMakeCurrentFn == nullptr) {
            return true;
        }

        if (eglMakeCurrentFn(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
            if (error) {
                *error = QStringLiteral("eglMakeCurrent(EGL_NO_CONTEXT) failed for the standalone runtime: %1")
                             .arg(eglErrorToString(resolveModuleSymbol<EGLint (EGLAPIENTRY *)(void)>(eglModule, "eglGetError")()));
            }
            return false;
        }

        return true;
    }

    void shutdown()
    {
        logStandaloneDiag(QStringLiteral("shutdown begin initialized=%1 eglDisplay=%2 eglContext=%3 eglSurface=%4 eglDevice=%5 d3d11Device=%6")
                              .arg(initialized)
                              .arg(pointerToString(eglDisplay))
                              .arg(pointerToString(eglContext))
                              .arg(pointerToString(eglSurface))
                              .arg(pointerToString(eglDevice))
                              .arg(pointerToString(d3d11Device.Get())));
        if (eglDisplay != EGL_NO_DISPLAY && eglMakeCurrentFn) {
            const EGLBoolean ok = eglMakeCurrentFn(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            logStandaloneDiag(QStringLiteral("shutdown eglMakeCurrent(no-context): %1")
                                  .arg(eglResultString(ok, eglModule)));
        }
        if (eglSurface != EGL_NO_SURFACE && eglDestroySurfaceFn && eglDisplay != EGL_NO_DISPLAY) {
            const EGLBoolean ok = eglDestroySurfaceFn(eglDisplay, eglSurface);
            logStandaloneDiag(QStringLiteral("shutdown eglDestroySurface: %1")
                                  .arg(eglResultString(ok, eglModule)));
        }
        if (eglContext != EGL_NO_CONTEXT && eglDestroyContextFn && eglDisplay != EGL_NO_DISPLAY) {
            const EGLBoolean ok = eglDestroyContextFn(eglDisplay, eglContext);
            logStandaloneDiag(QStringLiteral("shutdown eglDestroyContext: %1")
                                  .arg(eglResultString(ok, eglModule)));
        }
        if (eglDisplay != EGL_NO_DISPLAY && eglTerminateFn) {
            const EGLBoolean ok = eglTerminateFn(eglDisplay);
            logStandaloneDiag(QStringLiteral("shutdown eglTerminate: %1")
                                  .arg(eglResultString(ok, eglModule)));
        }
        if (eglDevice != EGL_NO_DEVICE_EXT) {
            logStandaloneDiag(QStringLiteral(
                                  "shutdown skip eglReleaseDeviceANGLE for external device=%1 because this ANGLE build also deletes the device from Display::~Display(), and explicit release here causes a double-destroy assert.")
                                  .arg(pointerToString(eglDevice)));
        }

        eglSurface = EGL_NO_SURFACE;
        eglContext = EGL_NO_CONTEXT;
        eglDisplay = EGL_NO_DISPLAY;
        eglDevice = EGL_NO_DEVICE_EXT;
        eglConfig = nullptr;
        d3d11DeviceContext.Reset();
        d3d11Device.Reset();
        adapterLuid.clear();
        glVendor.clear();
        glRenderer.clear();
        glVersion.clear();
        initialized = false;
        logStandaloneDiag(QStringLiteral("shutdown end"));
    }

    void *resolveProc(const char *name) const
    {
        if (name == nullptr) {
            return nullptr;
        }
        if (void *proc = reinterpret_cast<void *>(::GetProcAddress(glesModule, name))) {
            return proc;
        }
        if (void *proc = reinterpret_cast<void *>(::GetProcAddress(eglModule, name))) {
            return proc;
        }
        return eglGetProcAddressFn ? reinterpret_cast<void *>(eglGetProcAddressFn(name)) : nullptr;
    }

    bool resolveModules(QString *error)
    {
        const HMODULE eglDebugModule = ::GetModuleHandleW(L"libEGLd.dll");
        const HMODULE eglReleaseModule = ::GetModuleHandleW(L"libEGL.dll");
        const HMODULE glesDebugModule = ::GetModuleHandleW(L"libGLESv2d.dll");
        const HMODULE glesReleaseModule = ::GetModuleHandleW(L"libGLESv2.dll");

        if (eglDebugModule && glesDebugModule) {
            eglModule = eglDebugModule;
            glesModule = glesDebugModule;
        } else if (eglReleaseModule && glesReleaseModule) {
            eglModule = eglReleaseModule;
            glesModule = glesReleaseModule;
        } else {
            if (error) {
                *error = QStringLiteral("The Qt-owned ANGLE EGL/GLES modules are not loaded in the current process.");
            }
            return false;
        }

        eglModulePath = modulePathForHandle(eglModule);
        glesModulePath = modulePathForHandle(glesModule);
        return true;
    }

    bool resolveEglFunctions(QString *error)
    {
        eglGetProcAddressFn = resolveModuleSymbol<decltype(eglGetProcAddressFn)>(eglModule, "eglGetProcAddress");
        eglGetPlatformDisplayEXT = resolveEglSymbol<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglModule, eglGetProcAddressFn, "eglGetPlatformDisplayEXT");
        eglCreateDeviceANGLE = resolveEglSymbol<PFNEGLCREATEDEVICEANGLEPROC>(eglModule, eglGetProcAddressFn, "eglCreateDeviceANGLE");
        eglReleaseDeviceANGLE = resolveEglSymbol<PFNEGLRELEASEDEVICEANGLEPROC>(eglModule, eglGetProcAddressFn, "eglReleaseDeviceANGLE");
        eglQueryDisplayAttribEXT = resolveEglSymbol<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(eglModule, eglGetProcAddressFn, "eglQueryDisplayAttribEXT");
        eglQueryDeviceAttribEXT = resolveEglSymbol<PFNEGLQUERYDEVICEATTRIBEXTPROC>(eglModule, eglGetProcAddressFn, "eglQueryDeviceAttribEXT");
        eglInitializeFn = resolveEglSymbol<decltype(eglInitializeFn)>(eglModule, eglGetProcAddressFn, "eglInitialize");
        eglChooseConfigFn = resolveEglSymbol<decltype(eglChooseConfigFn)>(eglModule, eglGetProcAddressFn, "eglChooseConfig");
        eglCreateContextFn = resolveEglSymbol<decltype(eglCreateContextFn)>(eglModule, eglGetProcAddressFn, "eglCreateContext");
        eglCreatePbufferSurfaceFn = resolveEglSymbol<decltype(eglCreatePbufferSurfaceFn)>(eglModule, eglGetProcAddressFn, "eglCreatePbufferSurface");
        eglMakeCurrentFn = resolveEglSymbol<decltype(eglMakeCurrentFn)>(eglModule, eglGetProcAddressFn, "eglMakeCurrent");
        eglDestroyContextFn = resolveEglSymbol<decltype(eglDestroyContextFn)>(eglModule, eglGetProcAddressFn, "eglDestroyContext");
        eglDestroySurfaceFn = resolveEglSymbol<decltype(eglDestroySurfaceFn)>(eglModule, eglGetProcAddressFn, "eglDestroySurface");
        eglTerminateFn = resolveEglSymbol<decltype(eglTerminateFn)>(eglModule, eglGetProcAddressFn, "eglTerminate");

        if (!eglGetProcAddressFn || !eglGetPlatformDisplayEXT || !eglCreateDeviceANGLE || !eglReleaseDeviceANGLE
            || !eglQueryDisplayAttribEXT || !eglQueryDeviceAttribEXT || !eglInitializeFn || !eglChooseConfigFn
            || !eglCreateContextFn || !eglCreatePbufferSurfaceFn || !eglMakeCurrentFn || !eglDestroyContextFn
            || !eglDestroySurfaceFn || !eglTerminateFn) {
            if (error) {
                *error = QStringLiteral("The standalone ANGLE runtime could not resolve required EGL entry points.");
            }
            return false;
        }

        return true;
    }

    bool createStandaloneContext(QString *error)
    {
        if (!createD3D11Device(&d3d11Device, &d3d11DeviceContext, error)) {
            return false;
        }

        eglDevice = eglCreateDeviceANGLE(EGL_D3D11_DEVICE_ANGLE, d3d11Device.Get(), nullptr);
        if (eglDevice == EGL_NO_DEVICE_EXT) {
            if (error) {
                *error = QStringLiteral("eglCreateDeviceANGLE(EGL_D3D11_DEVICE_ANGLE) failed.");
            }
            return false;
        }

        eglDisplay = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, eglDevice, nullptr);
        if (eglDisplay == EGL_NO_DISPLAY) {
            if (error) {
                *error = QStringLiteral("eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, workerDevice) failed.");
            }
            return false;
        }

        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitializeFn(eglDisplay, &major, &minor) != EGL_TRUE) {
            if (error) {
                *error = QStringLiteral("eglInitialize failed for the standalone runtime.");
            }
            return false;
        }

        const EGLint configAttribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLint configCount = 0;
        if (eglChooseConfigFn(eglDisplay, configAttribs, &eglConfig, 1, &configCount) != EGL_TRUE
            || configCount <= 0 || eglConfig == nullptr) {
            if (error) {
                *error = QStringLiteral("eglChooseConfig failed for the standalone runtime.");
            }
            return false;
        }

        const EGLint pbufferAttribs[] = {
            EGL_WIDTH, 1,
            EGL_HEIGHT, 1,
            EGL_NONE
        };
        eglSurface = eglCreatePbufferSurfaceFn(eglDisplay, eglConfig, pbufferAttribs);
        if (eglSurface == EGL_NO_SURFACE) {
            if (error) {
                *error = QStringLiteral("eglCreatePbufferSurface failed for the standalone runtime.");
            }
            return false;
        }

        const EGLint contextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        eglContext = eglCreateContextFn(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttribs);
        if (eglContext == EGL_NO_CONTEXT) {
            if (error) {
                *error = QStringLiteral("eglCreateContext failed for the standalone runtime.");
            }
            return false;
        }

        Q_UNUSED(major);
        Q_UNUSED(minor);
        return true;
    }
};

AngleStandaloneRuntime::AngleStandaloneRuntime()
    : m_impl(std::make_unique<Impl>())
{
}

AngleStandaloneRuntime::~AngleStandaloneRuntime()
{
    shutdown();
}

bool AngleStandaloneRuntime::initialize(QString *error)
{
    return m_impl->initialize(error);
}

bool AngleStandaloneRuntime::enter(QString *error)
{
    return m_impl->makeCurrent(error);
}

void AngleStandaloneRuntime::leave()
{
    m_impl->doneCurrent(nullptr);
}

void AngleStandaloneRuntime::shutdown()
{
    m_impl->shutdown();
}

void *AngleStandaloneRuntime::resolveProc(const char *name) const
{
    return m_impl->resolveProc(name);
}

AngleStandaloneRuntime::RendererIdentity AngleStandaloneRuntime::queryRendererIdentity() const
{
    RendererIdentity identity;
    identity.eglModule = m_impl->eglModule;
    identity.eglModulePath = m_impl->eglModulePath;
    identity.glesModule = m_impl->glesModule;
    identity.glesModulePath = m_impl->glesModulePath;
    identity.eglDisplay = m_impl->eglDisplay;
    identity.eglDevice = m_impl->eglDevice;
    identity.d3d11Device = m_impl->d3d11Device.Get();
    identity.adapterLuid = m_impl->adapterLuid;
    identity.glVendor = m_impl->glVendor;
    identity.glRenderer = m_impl->glRenderer;
    identity.glVersion = m_impl->glVersion;

    return identity;
}

const Gles2ProcTable &AngleStandaloneRuntime::procTable() const
{
    return m_impl->procTable;
}

EGLDisplay AngleStandaloneRuntime::eglDisplay() const
{
    return m_impl->eglDisplay;
}

EGLConfig AngleStandaloneRuntime::eglConfig() const
{
    return m_impl->eglConfig;
}

ID3D11Device *AngleStandaloneRuntime::d3d11Device() const
{
    return m_impl->d3d11Device.Get();
}

ID3D11DeviceContext *AngleStandaloneRuntime::d3d11DeviceContext() const
{
    return m_impl->d3d11DeviceContext.Get();
}
