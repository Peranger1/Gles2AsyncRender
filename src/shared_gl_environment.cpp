#include "shared_gl_environment.h"

#include "shared_gl_context_handle.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>

SharedGlEnvironment::SharedGlEnvironment(QObject *parent)
    : QObject(parent)
{
}

SharedGlEnvironment::~SharedGlEnvironment()
{
    for (QOffscreenSurface *surface : m_surfaces) {
        if (surface != nullptr) {
            surface->destroy();
            delete surface;
        }
    }
    m_surfaces.clear();
}

bool SharedGlEnvironment::initializeFromDisplay(QOpenGLContext *displayContext, const QSurfaceFormat &format)
{
    if (displayContext == nullptr) {
        return false;
    }

    m_displayContext = displayContext;
    m_format = format;
    return true;
}

SharedGlContextHandle *SharedGlEnvironment::createSharedContext(QObject *parent)
{
    if (m_displayContext == nullptr) {
        return nullptr;
    }

    QOpenGLContext *context = new QOpenGLContext();
    context->setFormat(m_format);
    context->setShareContext(m_displayContext);
    context->setScreen(m_displayContext->screen());
    if (!context->create()) {
        delete context;
        return nullptr;
    }

    QOffscreenSurface *surface = new QOffscreenSurface(m_displayContext->screen());
    surface->setFormat(m_format);
    surface->create();
    if (!surface->isValid()) {
        delete surface;
        delete context;
        return nullptr;
    }

    m_surfaces.push_back(surface);

    SharedGlContextHandle *handle = new SharedGlContextHandle(parent);
    handle->adopt(context, surface);
    return handle;
}

bool SharedGlEnvironment::isInitialized() const
{
    return m_displayContext != nullptr;
}
