#pragma once

#include <QString>

#include <QtANGLE/EGL/egl.h>
#include <QtANGLE/EGL/eglext.h>
#include <QtANGLE/GLES2/gl2.h>

#include <functional>

#define GLES2_PROC_TABLE_REQUIRED_EGL_SYMBOLS(X) \
    X(eglGetProcAddress, __eglMustCastToProperFunctionPointerType (EGLAPIENTRY *)(const char *)) \
    X(eglGetCurrentContext, EGLContext (EGLAPIENTRY *)(void)) \
    X(eglGetCurrentDisplay, EGLDisplay (EGLAPIENTRY *)(void)) \
    X(eglGetError, EGLint (EGLAPIENTRY *)(void)) \
    X(eglQueryString, const char * (EGLAPIENTRY *)(EGLDisplay, EGLint)) \
    X(eglChooseConfig, EGLBoolean (EGLAPIENTRY *)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)) \
    X(eglDestroySurface, EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLSurface))

#define GLES2_PROC_TABLE_OPTIONAL_EGL_SYMBOLS(X) \
    X(eglCreatePbufferFromClientBuffer, EGLSurface (EGLAPIENTRY *)(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig, const EGLint *)) \
    X(eglBindTexImage, EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLSurface, EGLint)) \
    X(eglReleaseTexImage, EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLSurface, EGLint)) \
    X(eglQuerySurfacePointerANGLE, PFNEGLQUERYSURFACEPOINTERANGLEPROC)

#define GLES2_PROC_TABLE_GL_SYMBOLS(X) \
    X(glGetString, const GLubyte * (GL_APIENTRYP)(GLenum)) \
    X(glGetError, GLenum (GL_APIENTRYP)(void)) \
    X(glPixelStorei, void (GL_APIENTRYP)(GLenum, GLint)) \
    X(glGenTextures, void (GL_APIENTRYP)(GLsizei, GLuint *)) \
    X(glDeleteTextures, void (GL_APIENTRYP)(GLsizei, const GLuint *)) \
    X(glBindTexture, void (GL_APIENTRYP)(GLenum, GLuint)) \
    X(glTexParameteri, void (GL_APIENTRYP)(GLenum, GLenum, GLint)) \
    X(glTexImage2D, void (GL_APIENTRYP)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(glGenFramebuffers, void (GL_APIENTRYP)(GLsizei, GLuint *)) \
    X(glDeleteFramebuffers, void (GL_APIENTRYP)(GLsizei, const GLuint *)) \
    X(glBindFramebuffer, void (GL_APIENTRYP)(GLenum, GLuint)) \
    X(glFramebufferTexture2D, void (GL_APIENTRYP)(GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(glCheckFramebufferStatus, GLenum (GL_APIENTRYP)(GLenum)) \
    X(glViewport, void (GL_APIENTRYP)(GLint, GLint, GLsizei, GLsizei)) \
    X(glClearColor, void (GL_APIENTRYP)(GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glClear, void (GL_APIENTRYP)(GLbitfield)) \
    X(glCreateShader, GLuint (GL_APIENTRYP)(GLenum)) \
    X(glShaderSource, void (GL_APIENTRYP)(GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(glCompileShader, void (GL_APIENTRYP)(GLuint)) \
    X(glGetShaderiv, void (GL_APIENTRYP)(GLuint, GLenum, GLint *)) \
    X(glGetShaderInfoLog, void (GL_APIENTRYP)(GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(glDeleteShader, void (GL_APIENTRYP)(GLuint)) \
    X(glCreateProgram, GLuint (GL_APIENTRYP)(void)) \
    X(glAttachShader, void (GL_APIENTRYP)(GLuint, GLuint)) \
    X(glLinkProgram, void (GL_APIENTRYP)(GLuint)) \
    X(glGetProgramiv, void (GL_APIENTRYP)(GLuint, GLenum, GLint *)) \
    X(glGetProgramInfoLog, void (GL_APIENTRYP)(GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(glDeleteProgram, void (GL_APIENTRYP)(GLuint)) \
    X(glUseProgram, void (GL_APIENTRYP)(GLuint)) \
    X(glGetAttribLocation, GLint (GL_APIENTRYP)(GLuint, const GLchar *)) \
    X(glGetUniformLocation, GLint (GL_APIENTRYP)(GLuint, const GLchar *)) \
    X(glUniform1i, void (GL_APIENTRYP)(GLint, GLint)) \
    X(glUniform1f, void (GL_APIENTRYP)(GLint, GLfloat)) \
    X(glUniform2f, void (GL_APIENTRYP)(GLint, GLfloat, GLfloat)) \
    X(glVertexAttribPointer, void (GL_APIENTRYP)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(glEnableVertexAttribArray, void (GL_APIENTRYP)(GLuint)) \
    X(glDisableVertexAttribArray, void (GL_APIENTRYP)(GLuint)) \
    X(glActiveTexture, void (GL_APIENTRYP)(GLenum)) \
    X(glDrawArrays, void (GL_APIENTRYP)(GLenum, GLint, GLsizei)) \
    X(glFlush, void (GL_APIENTRYP)(void)) \
    X(glReadPixels, void (GL_APIENTRYP)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))

class Gles2ProcTable final
{
public:
    using ResolveProc = std::function<void *(const char *name)>;

    bool load(const ResolveProc &resolver, QString *error);
    bool isValid() const;
    bool supportsAngleD3DTextureImport() const;

#define GLES2_DECLARE_PROC(name, type) using name##Proc = type; name##Proc name = nullptr;
    GLES2_PROC_TABLE_REQUIRED_EGL_SYMBOLS(GLES2_DECLARE_PROC)
    GLES2_PROC_TABLE_OPTIONAL_EGL_SYMBOLS(GLES2_DECLARE_PROC)
    GLES2_PROC_TABLE_GL_SYMBOLS(GLES2_DECLARE_PROC)
#undef GLES2_DECLARE_PROC
};
