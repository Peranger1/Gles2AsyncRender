#pragma once

#include <QOpenGLContext>
#include <QString>
#include <QStringList>

#include <QtANGLE/EGL/egl.h>
#include <QtANGLE/EGL/eglext.h>
#include <QtANGLE/EGL/eglext_angle.h>

struct ID3D11Device;

namespace QtAngleEglTools
{
struct ResolvedEglApi final
{
    void *module = nullptr;
    QString modulePath;

    EGLint (EGLAPIENTRY *getError)(void) = nullptr;
    const char * (EGLAPIENTRY *queryString)(EGLDisplay, EGLint) = nullptr;
    EGLDisplay (EGLAPIENTRY *getCurrentDisplay)(void) = nullptr;
    __eglMustCastToProperFunctionPointerType (EGLAPIENTRY *getProcAddress)(const char *) = nullptr;
    PFNEGLQUERYDISPLAYATTRIBEXTPROC queryDisplayAttrib = nullptr;
    PFNEGLQUERYDEVICEATTRIBEXTPROC queryDeviceAttrib = nullptr;
    EGLSurface (EGLAPIENTRY *createPbufferFromClientBuffer)(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig, const EGLint *) = nullptr;
    EGLBoolean (EGLAPIENTRY *destroySurface)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (EGLAPIENTRY *bindTexImage)(EGLDisplay, EGLSurface, EGLint) = nullptr;
    EGLBoolean (EGLAPIENTRY *releaseTexImage)(EGLDisplay, EGLSurface, EGLint) = nullptr;
    EGLBoolean (EGLAPIENTRY *chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = nullptr;
    EGLBoolean (EGLAPIENTRY *getConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *) = nullptr;
    PFNEGLQUERYSURFACEPOINTERANGLEPROC querySurfacePointerANGLE = nullptr;

    bool isValid() const;
    bool supportsD3DTextureImport() const;
};

struct RendererIdentity final
{
    void *eglModule = nullptr;
    QString eglModulePath;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLDeviceEXT eglDevice = EGL_NO_DEVICE_EXT;
    ID3D11Device *d3d11Device = nullptr;
    QString adapterLuid;
    QString eglVendor;
    QString eglVersion;
    QString glVendor;
    QString glRenderer;
    QString glVersion;
};

QString eglErrorToString(EGLint error);
QString pointerToString(const void *value);
QString pointerToString(quintptr value);

ResolvedEglApi resolveEglApi(QOpenGLContext *context, QStringList *probeLog = nullptr);
bool isUsableDisplay(EGLDisplay display, const ResolvedEglApi &api, QString *details = nullptr);
EGLDisplay queryDisplay(QOpenGLContext *context, const ResolvedEglApi &api, QStringList *probeLog = nullptr);
EGLConfig queryConfig(QOpenGLContext *context, QStringList *probeLog = nullptr);
RendererIdentity queryRendererIdentity(QOpenGLContext *context,
                                       EGLDisplay display,
                                       const ResolvedEglApi &api,
                                       QStringList *probeLog = nullptr);
} // namespace QtAngleEglTools
