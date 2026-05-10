#pragma once

#include "gles2_proc_table.h"

#include <QString>

#include <QtANGLE/EGL/egl.h>
#include <QtANGLE/EGL/eglext.h>
#include <QtANGLE/EGL/eglext_angle.h>

#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;

class AngleStandaloneRuntime final
{
public:
    struct RendererIdentity final
    {
        void *eglModule = nullptr;
        QString eglModulePath;
        void *glesModule = nullptr;
        QString glesModulePath;
        EGLDisplay eglDisplay = EGL_NO_DISPLAY;
        EGLDeviceEXT eglDevice = EGL_NO_DEVICE_EXT;
        ID3D11Device *d3d11Device = nullptr;
        QString adapterLuid;
        QString glVendor;
        QString glRenderer;
        QString glVersion;
    };

    AngleStandaloneRuntime();
    ~AngleStandaloneRuntime();

    bool initialize(QString *error);
    bool makeCurrent(QString *error);
    bool doneCurrent(QString *error);
    void shutdown();

    void *resolveProc(const char *name) const;
    RendererIdentity queryRendererIdentity() const;

    const Gles2ProcTable &procTable() const;
    ID3D11Device *d3d11Device() const;
    ID3D11DeviceContext *d3d11DeviceContext() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

