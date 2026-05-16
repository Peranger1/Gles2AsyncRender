#pragma once

#include "framework/platform/runtime.h"

#include <QtANGLE/EGL/egl.h>

#include <memory>

class Gles2ProcTable;
struct ID3D11Device;
struct ID3D11DeviceContext;
class WinAngleTextureWriter;

class WinAngleRuntime final : public IRuntime
{
public:
    WinAngleRuntime();
    ~WinAngleRuntime() override;

    bool initialize(QString *error) override;
    bool enter(QString *error) override;
    void leave() override;
    void shutdown() override;
    void *resolveProc(const char *name) const override;

private:
    const Gles2ProcTable *procTable() const noexcept;
    EGLDisplay eglDisplay() const noexcept;
    EGLConfig eglConfig() const noexcept;
    ID3D11Device *d3d11Device() const noexcept;
    ID3D11DeviceContext *d3d11DeviceContext() const noexcept;

    friend class WinAngleTextureWriter;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
