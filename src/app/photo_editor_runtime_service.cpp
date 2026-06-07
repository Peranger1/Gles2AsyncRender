#include "photo_editor_runtime_service.h"

#include "app/photo_editor_handle_actor.h"
#include "framework/execution/runtime_executor.h"
#include "framework/execution/runtime_scope.h"
#include "framework/platform/runtime.h"
#include "photo_editor/photo_editor_gles2_backend.h"
#include "runtime_diagnostics.h"

#include <QMutexLocker>
#include <QPointer>

#include <exception>

namespace
{
constexpr const char *kLogScope = "[PhotoEditorRuntimeService]";
thread_local IRuntime *g_photoEditorInitRuntime = nullptr;

QString exceptionMessage(std::exception_ptr exception, const QString &fallback)
{
    if (!exception) {
        return fallback;
    }

    try {
        std::rethrow_exception(exception);
    } catch (const std::exception &ex) {
        const QString message = QString::fromStdString(ex.what());
        return message.isEmpty() ? fallback : message;
    } catch (...) {
        return fallback;
    }
}
}

PhotoEditorRuntimeService::PhotoEditorRuntimeService(execution::RuntimeExecutor *executor, QObject *parent)
    : QObject(parent)
    , m_executor(executor)
{
}

PhotoEditorRuntimeService::~PhotoEditorRuntimeService()
{
    shutdown();
}

void PhotoEditorRuntimeService::initialize()
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_shuttingDown || m_initializePosted || m_initialized) {
            return;
        }
        m_initializePosted = true;
    }

    if (m_executor == nullptr) {
        {
            QMutexLocker locker(&m_mutex);
            m_initializePosted = false;
        }
        emitWarning(QStringLiteral("Photo editor runtime service requires a runtime executor."));
        return;
    }

    QPointer<PhotoEditorRuntimeService> service(this);
    auto future = m_executor->submit([service](IRuntime &runtime) {
        if (service.isNull()) {
            return;
        }
        RuntimeDiagnostics::logInfo(kLogScope, QStringLiteral("[GL] service photo_editor_init"));

        QString errorText;
        RuntimeScope scope(&runtime, &errorText);
        bool ok = false;
        if (scope.ok()) {
            g_photoEditorInitRuntime = &runtime;
            ok = photo_editor_init([](const char *name) -> void * {
                return g_photoEditorInitRuntime ? g_photoEditorInitRuntime->resolveProc(name) : nullptr;
            }, &errorText);
            g_photoEditorInitRuntime = nullptr;
        }

        {
            QMutexLocker locker(&service->m_mutex);
            service->m_initialized = ok;
            if (!ok) {
                service->m_initializePosted = false;
            }
        }

        if (!ok) {
            service->emitWarning(errorText.isEmpty()
                                     ? QStringLiteral("photo_editor_init failed.")
                                     : errorText);
        }
    });

    std::move(future).thenTry([service](async::Try<async::Unit> &&result) {
        if (service.isNull() || !result.hasException()) {
            return async::Unit();
        }

        {
            QMutexLocker locker(&service->m_mutex);
            service->m_initializePosted = false;
            service->m_initialized = false;
        }
        service->emitWarning(exceptionMessage(result.exception(),
                                              QStringLiteral("Failed to submit photo_editor_init task.")));
        return async::Unit();
    });
}

std::shared_ptr<PhotoEditorHandleActor> PhotoEditorRuntimeService::createActor(QString sourceKey,
                                                                               quint64 sourceImageCacheKey,
                                                                               std::shared_ptr<const QImage> sourceImage)
{
    initialize();

    std::shared_ptr<PhotoEditorHandleActor> actor;
    {
        QMutexLocker locker(&m_mutex);
        if (m_shuttingDown) {
            emitWarning(QStringLiteral("Cannot create a photo editor actor while the runtime service is shutting down."));
            return {};
        }

        const quint64 actorId = ++m_nextActorId;
        actor = std::make_shared<PhotoEditorHandleActor>(actorId,
                                                         m_executor,
                                                         std::move(sourceKey),
                                                         sourceImageCacheKey,
                                                         std::move(sourceImage));
        m_actors.insert(actorId, actor);
    }

    connect(actor.get(), &PhotoEditorHandleActor::warning, this, &PhotoEditorRuntimeService::warning);
    actor->create();
    return actor;
}

void PhotoEditorRuntimeService::shutdown()
{
    QHash<quint64, std::shared_ptr<PhotoEditorHandleActor>> actors;
    {
        QMutexLocker locker(&m_mutex);
        if (m_shuttingDown) {
            return;
        }
        m_shuttingDown = true;
        actors = m_actors;
        m_actors.clear();
    }

    for (const std::shared_ptr<PhotoEditorHandleActor> &actor : actors) {
        if (actor) {
            QString error;
            if (!actor->destroySync(&error)) {
                emitWarning(error.isEmpty()
                                ? QStringLiteral("Failed to synchronously destroy a photo editor actor.")
                                : error);
            }
        }
    }
}

void PhotoEditorRuntimeService::emitWarning(const QString &message)
{
    if (message.isEmpty()) {
        return;
    }

    RuntimeDiagnostics::logWarning(kLogScope, message);
    emit warning(message);
}
