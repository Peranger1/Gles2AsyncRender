#include "mac_cocoa_gl_runtime.h"

#include "mac_cocoa_gl_shared_state.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QString>

MacCocoaGlRuntime::MacCocoaGlRuntime(const std::shared_ptr<MacCocoaGlSharedState> &sharedState)
    : m_sharedState(sharedState)
{
}

MacCocoaGlRuntime::~MacCocoaGlRuntime()
{
    shutdown();
}

bool MacCocoaGlRuntime::initialize(QString *error)
{
    if (m_initialized) {
        return true;
    }
    if (!m_sharedState || !m_sharedState->offscreenSurface
        || !m_sharedState->offscreenSurface->isValid()) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlRuntime requires prepared compatibility parameters and an offscreen surface.");
        }
        return false;
    }

    m_context = std::make_unique<QOpenGLContext>();
    m_context->setFormat(m_sharedState->format);
    if (m_sharedState->screen) {
        m_context->setScreen(m_sharedState->screen);
    }
    if (!m_context->create()) {
        m_context.reset();
        if (error) {
            *error = QStringLiteral("MacCocoaGlRuntime failed to create the standalone worker QOpenGLContext.");
        }
        return false;
    }

    m_initialized = true;
    return true;
}

bool MacCocoaGlRuntime::enter(QString *error)
{
    if (!m_initialized || !m_context || !m_sharedState || !m_sharedState->offscreenSurface) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlRuntime is not initialized.");
        }
        return false;
    }

    if (!m_context->makeCurrent(m_sharedState->offscreenSurface.get())) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlRuntime failed to make the worker context current.");
        }
        return false;
    }
    return true;
}

void MacCocoaGlRuntime::leave()
{
    if (m_context) {
        m_context->doneCurrent();
    }
}

void MacCocoaGlRuntime::shutdown()
{
    if (m_context && QOpenGLContext::currentContext() == m_context.get()) {
        m_context->doneCurrent();
    }
    m_context.reset();
    m_initialized = false;
}

void *MacCocoaGlRuntime::resolveProc(const char *name) const
{
    if (!m_context || name == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void *>(m_context->getProcAddress(name));
}
