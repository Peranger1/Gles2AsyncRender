#include "shared_gl_context_handle.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>

SharedGlContextHandle::SharedGlContextHandle(QObject *parent)
    : QObject(parent)
{
}

SharedGlContextHandle::~SharedGlContextHandle()
{
    shutdown();
}

void SharedGlContextHandle::adopt(QOpenGLContext *context, QOffscreenSurface *surface)
{
    shutdown();
    m_context = context;
    m_surface = surface;
}

QOpenGLContext *SharedGlContextHandle::context() const
{
    return m_context;
}

QOffscreenSurface *SharedGlContextHandle::surface() const
{
    return m_surface;
}

bool SharedGlContextHandle::makeCurrent()
{
    return m_context != nullptr
        && m_surface != nullptr
        && m_context->makeCurrent(m_surface);
}

void SharedGlContextHandle::doneCurrent()
{
    if (m_context != nullptr) {
        m_context->doneCurrent();
    }
}

void SharedGlContextHandle::shutdown()
{
    if (m_context != nullptr) {
        m_context->doneCurrent();
        delete m_context;
        m_context = nullptr;
    }

    m_surface = nullptr;
}
