#include "photo_editor_runtime_service.h"

#include "app/photo_editor_handle_actor.h"
#include "framework/execution/runtime_executor.h"
#include "framework/execution/runtime_scope.h"
#include "framework/platform/runtime.h"
#include "photo_editor/photo_editor_gles2_backend.h"
#include "runtime_diagnostics.h"

#include <QMutexLocker>
#include <QPointer>

namespace
{
constexpr const char *kLogScope = "[PhotoEditorRuntimeService]";
thread_local IRuntime *g_photoEditorInitRuntime = nullptr;
}

PhotoEditorRuntimeService::PhotoEditorRuntimeService(RuntimeHost *host, QObject *parent)
    : QObject(parent)
    , m_executor(std::make_unique<RuntimeExecutor>(host))
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

    QPointer<PhotoEditorRuntimeService> service(this);
    const SubmitResult submit = m_executor->post([service](IRuntime *runtime) {
        if (service.isNull()) {
            return;
        }
        RuntimeDiagnostics::logInfo(kLogScope, QStringLiteral("[GL] service photo_editor_init"));

        QString errorText;
        RuntimeScope scope(runtime, &errorText);
        bool ok = false;
        if (scope.ok()) {
            g_photoEditorInitRuntime = runtime;
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

    if (!submit.accepted) {
        {
            QMutexLocker locker(&m_mutex);
            m_initializePosted = false;
        }
        emitWarning(submit.error.message.isEmpty()
                        ? QStringLiteral("Failed to post photo_editor_init task.")
                        : submit.error.message);
    }
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
                                                         m_executor.get(),
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

    if (m_executor) {
        m_executor->shutdown();
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
