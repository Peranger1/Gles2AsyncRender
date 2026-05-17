#include "photo_editor_runtime_host.h"

#include "framework/platform/runtime.h"

#include <QMetaObject>
#include <QThread>

PhotoEditorRuntimeHost::PhotoEditorRuntimeHost(std::unique_ptr<IRuntime> runtime, QObject *parent)
    : QObject(parent)
    , m_runtime(std::move(runtime))
{
}

PhotoEditorRuntimeHost::~PhotoEditorRuntimeHost()
{
    shutdown();
}

bool PhotoEditorRuntimeHost::start(QString *error)
{
    if (m_started) {
        return true;
    }
    if (!m_runtime) {
        if (error) {
            *error = QStringLiteral("PhotoEditorRuntimeHost requires a valid runtime.");
        }
        return false;
    }
    if (!m_runtime->initialize(error)) {
        return false;
    }
    m_started = true;
    m_shuttingDown = false;
    return true;
}

void PhotoEditorRuntimeHost::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    if (m_runtime) {
        m_runtime->shutdown();
    }
    m_started = false;
}

IRuntime *PhotoEditorRuntimeHost::runtime() const
{
    return m_runtime.get();
}

bool PhotoEditorRuntimeHost::isOnRuntimeThread() const
{
    return QThread::currentThread() == thread();
}

bool PhotoEditorRuntimeHost::dispatchAsync(RuntimeClosure closure, QString *error)
{
    if (!m_started || !m_runtime || !closure) {
        if (error) {
            *error = QStringLiteral("PhotoEditorRuntimeHost cannot dispatch an async closure.");
        }
        return false;
    }

    if (isOnRuntimeThread()) {
        closure(m_runtime.get());
        return true;
    }

    return QMetaObject::invokeMethod(this, [this, closure = std::move(closure)]() {
        closure(m_runtime.get());
    }, Qt::QueuedConnection);
}

bool PhotoEditorRuntimeHost::dispatchSync(RuntimeClosure closure, QString *error)
{
    if (!m_started || !m_runtime || !closure) {
        if (error) {
            *error = QStringLiteral("PhotoEditorRuntimeHost cannot dispatch a sync closure.");
        }
        return false;
    }

    if (isOnRuntimeThread()) {
        closure(m_runtime.get());
        return true;
    }

    const bool ok = QMetaObject::invokeMethod(this, [this, closure = std::move(closure)]() {
        closure(m_runtime.get());
    }, Qt::BlockingQueuedConnection);
    if (!ok && error) {
        *error = QStringLiteral("QMetaObject::invokeMethod failed while dispatching synchronously.");
    }
    return ok;
}
