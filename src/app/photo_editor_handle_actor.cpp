#include "photo_editor_handle_actor.h"

#include "framework/execution/runtime_executor.h"
#include "framework/execution/runtime_scope.h"
#include "framework/platform/runtime.h"
#include "photo_editor/photo_editor_gles2_backend.h"
#include "runtime_diagnostics.h"

#include <QMutexLocker>
#include <QVariant>

namespace
{
constexpr const char *kLogScope = "[PhotoEditorHandleActor]";

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

QString emptyErrorFallback(const QString &message, const QString &fallback)
{
    return message.isEmpty() ? fallback : message;
}
}

PhotoEditorHandleActor::PhotoEditorHandleActor(quint64 actorId,
                                               RuntimeExecutor *executor,
                                               QString sourceKey,
                                               quint64 sourceImageCacheKey,
                                               std::shared_ptr<const QImage> sourceImage,
                                               QObject *parent)
    : QObject(parent)
    , m_actorId(actorId)
    , m_executor(executor)
    , m_sourceKey(std::move(sourceKey))
    , m_sourceImageCacheKey(sourceImageCacheKey)
    , m_sourceImage(std::move(sourceImage))
{
}

PhotoEditorHandleActor::~PhotoEditorHandleActor()
{
    destroy();
}

quint64 PhotoEditorHandleActor::actorId() const
{
    QMutexLocker locker(&m_mutex);
    return m_actorId;
}

QString PhotoEditorHandleActor::sourceKey() const
{
    QMutexLocker locker(&m_mutex);
    return m_sourceKey;
}

quint64 PhotoEditorHandleActor::sourceImageCacheKey() const
{
    QMutexLocker locker(&m_mutex);
    return m_sourceImageCacheKey;
}

PhotoEditorHandleActor::State PhotoEditorHandleActor::state() const
{
    QMutexLocker locker(&m_mutex);
    return m_state;
}

void PhotoEditorHandleActor::create()
{
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_destroyRequested || m_state != State::Empty) {
            return;
        }
        m_state = State::Creating;
        generation = ++m_generation;
    }

    postCreateTask(generation);
}

void PhotoEditorHandleActor::setOutputSize(QSize size)
{
    bool shouldPost = false;
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_destroyRequested || m_state == State::Destroying || m_state == State::Destroyed) {
            return;
        }

        m_outputSize = sanitizedSize(size);
        m_hasOutputSize = true;
        generation = m_generation;
        if (canUseHandleLocked() && !m_outputSizeTaskPending) {
            m_outputSizeTaskPending = true;
            shouldPost = true;
        }
    }

    if (shouldPost) {
        postSetOutputSizeTask(generation);
    }
}

void PhotoEditorHandleActor::setOpcode(ImageEffectParameters parameters)
{
    bool shouldPost = false;
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_destroyRequested || m_state == State::Destroying || m_state == State::Destroyed) {
            return;
        }

        m_latestParameters = parameters;
        m_hasOpcode = true;
        generation = m_generation;
        if (canUseHandleLocked() && !m_opcodeTaskPending) {
            m_opcodeTaskPending = true;
            shouldPost = true;
        }
    }

    if (shouldPost) {
        postSetOpcodeTask(generation);
    }
}

void PhotoEditorHandleActor::process()
{
    bool shouldPost = false;
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_destroyRequested || m_state == State::Destroying || m_state == State::Destroyed) {
            return;
        }
        if (!canUseHandleLocked()) {
            m_processAgainRequested = true;
            return;
        }
        if (m_state == State::Processing || m_state == State::Rendering || m_processTaskPending) {
            m_processAgainRequested = true;
            return;
        }

        m_state = State::Processing;
        m_processTaskPending = true;
        generation = m_generation;
        shouldPost = true;
    }

    if (shouldPost) {
        postProcessTask(generation);
    }
}

void PhotoEditorHandleActor::render()
{
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (!isCurrentLocked(m_generation) || m_destroyRequested || m_state == State::Destroying || m_state == State::Destroyed) {
            return;
        }
        if (m_handle == nullptr || (m_state != State::Processed && m_state != State::Rendered)) {
            return;
        }

        m_state = State::Rendering;
        generation = m_generation;
    }

    postRenderTask(generation);
}

void PhotoEditorHandleActor::destroy()
{
    void *handle = nullptr;
    quint64 generation = 0;
    bool shouldPost = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_state == State::Destroyed || m_state == State::Destroying) {
            return;
        }

        m_destroyRequested = true;
        generation = ++m_generation;
        handle = m_handle;
        m_handle = nullptr;
        m_activeProcessGeneration = 0;

        if (handle == nullptr) {
            m_state = (m_state == State::Creating) ? State::Destroying : State::Destroyed;
            return;
        }

        m_state = State::Destroying;
        shouldPost = true;
    }

    if (shouldPost) {
        postDestroyTask(handle, generation);
    }
}

bool PhotoEditorHandleActor::destroySync(QString *error)
{
    if (m_executor == nullptr) {
        if (error) {
            *error = QStringLiteral("Photo editor actor cannot destroy synchronously without a runtime executor.");
        }
        return false;
    }

    QString barrierError;
    if (!m_executor->call([](IRuntime *) {}, &barrierError)) {
        if (error) {
            *error = emptyErrorFallback(barrierError,
                                        QStringLiteral("Failed to drain photo editor actor tasks before destroy."));
        }
        return false;
    }

    void *handle = nullptr;
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_state == State::Destroyed) {
            return true;
        }

        m_destroyRequested = true;
        generation = ++m_generation;
        handle = m_handle;
        m_handle = nullptr;
        m_activeProcessGeneration = 0;

        if (handle == nullptr) {
            m_state = State::Destroyed;
            return true;
        }

        m_state = State::Destroying;
    }

    const quint64 actorId = m_actorId;
    QString callError;
    QString destroyError;
    bool didDestroy = false;
    const bool called = m_executor->call([handle, actorId, generation, &didDestroy, &destroyError](IRuntime *runtime) {
        RuntimeDiagnostics::logInfo(kLogScope,
                                    QStringLiteral("[GL] actor#%1 gen=%2 photo_editor_destroy")
                                        .arg(actorId)
                                        .arg(generation));

        RuntimeScope scope(runtime, &destroyError);
        if (!scope.ok()) {
            return;
        }
        photo_editor_destroy(handle);
        didDestroy = true;
    }, &callError);

    {
        QMutexLocker locker(&m_mutex);
        if (didDestroy) {
            m_state = State::Destroyed;
        } else {
            m_handle = handle;
            m_state = State::Failed;
        }
    }

    if (!called || !didDestroy) {
        if (error) {
            const QString message = !called ? callError : destroyError;
            *error = emptyErrorFallback(message,
                                        QStringLiteral("Failed to synchronously destroy the photo editor actor with a current runtime scope."));
        }
        return false;
    }
    return true;
}

void PhotoEditorHandleActor::postCreateTask(quint64 generation)
{
    if (m_executor == nullptr) {
        failIfCurrent(generation, QStringLiteral("Photo editor actor requires a runtime executor."));
        return;
    }

    const auto weakActor = weak_from_this();
    const SubmitResult submit = m_executor->post([weakActor, generation](IRuntime *runtime) {
        const auto self = weakActor.lock();
        if (!self) {
            return;
        }

        std::shared_ptr<const QImage> sourceImage;
        {
            QMutexLocker locker(&self->m_mutex);
            if (!self->isCurrentLocked(generation) || self->m_state != State::Creating || self->m_destroyRequested) {
                RuntimeDiagnostics::logDiag(kLogScope,
                                            QStringLiteral("[GL] actor#%1 gen=%2 drop photo_editor_create state=%3")
                                                .arg(self->m_actorId)
                                                .arg(generation)
                                                .arg(stateName(self->m_state)));
                return;
            }
            sourceImage = self->m_sourceImage;
        }

        RuntimeDiagnostics::logInfo(kLogScope,
                                    QStringLiteral("[GL] actor#%1 gen=%2 photo_editor_create")
                                        .arg(self->m_actorId)
                                        .arg(generation));

        QString errorText;
        RuntimeScope scope(runtime, &errorText);
        void *createdHandle = nullptr;
        if (scope.ok()) {
            createdHandle = photo_editor_create(sourceImage ? *sourceImage : QImage(), &errorText);
        }

        bool postSize = false;
        bool postOpcode = false;
        bool postProcess = false;
        void *orphanHandle = nullptr;
        quint64 destroyGeneration = 0;
        {
            QMutexLocker locker(&self->m_mutex);
            if (!self->isCurrentLocked(generation) || self->m_destroyRequested) {
                orphanHandle = createdHandle;
                destroyGeneration = self->m_generation;
            } else if (!scope.ok() || createdHandle == nullptr) {
                self->m_state = State::Failed;
            } else {
                self->m_handle = createdHandle;
                self->m_state = State::Ready;
                if (self->m_hasOutputSize && !self->m_outputSizeTaskPending) {
                    self->m_outputSizeTaskPending = true;
                    postSize = true;
                }
                if (self->m_hasOpcode && !self->m_opcodeTaskPending) {
                    self->m_opcodeTaskPending = true;
                    postOpcode = true;
                }
                if (self->m_processAgainRequested && !self->m_processTaskPending) {
                    self->m_processAgainRequested = false;
                    self->m_state = State::Processing;
                    self->m_processTaskPending = true;
                    postProcess = true;
                }
            }
        }

        if (!scope.ok() || (createdHandle == nullptr && orphanHandle == nullptr)) {
            self->warn(emptyErrorFallback(errorText, QStringLiteral("photo_editor_create failed.")));
        }
        if (orphanHandle != nullptr) {
            self->postDestroyTask(orphanHandle, destroyGeneration);
            return;
        }
        if (postSize) {
            self->postSetOutputSizeTask(generation);
        }
        if (postOpcode) {
            self->postSetOpcodeTask(generation);
        }
        if (postProcess) {
            self->postProcessTask(generation);
        }
    });

    if (!submit.accepted) {
        failIfCurrent(generation, submit.error.message);
    }
}

void PhotoEditorHandleActor::postSetOutputSizeTask(quint64 generation)
{
    if (m_executor == nullptr) {
        failIfCurrent(generation, QStringLiteral("Photo editor actor requires a runtime executor."));
        return;
    }

    const auto weakActor = weak_from_this();
    const SubmitResult submit = m_executor->post([weakActor, generation](IRuntime *runtime) {
        const auto self = weakActor.lock();
        if (!self) {
            return;
        }

        void *handle = nullptr;
        QSize outputSize;
        {
            QMutexLocker locker(&self->m_mutex);
            if (!self->isCurrentLocked(generation) || !self->canUseHandleLocked() || !self->m_hasOutputSize) {
                self->m_outputSizeTaskPending = false;
                return;
            }
            handle = self->m_handle;
            outputSize = self->m_outputSize;
        }

        RuntimeDiagnostics::logInfo(kLogScope,
                                    QStringLiteral("[GL] actor#%1 gen=%2 photo_editor_set_output_size")
                                        .arg(self->m_actorId)
                                        .arg(generation));

        QString errorText;
        RuntimeScope scope(runtime, &errorText);
        const bool ok = scope.ok() && photo_editor_set_output_size(handle, outputSize, &errorText);

        bool repost = false;
        {
            QMutexLocker locker(&self->m_mutex);
            if (self->isCurrentLocked(generation)) {
                if (!ok) {
                    self->m_state = State::Failed;
                }
                self->m_outputSizeTaskPending = false;
                if (ok && self->canUseHandleLocked() && self->m_hasOutputSize && self->m_outputSize != outputSize) {
                    self->m_outputSizeTaskPending = true;
                    repost = true;
                }
            }
        }

        if (!ok) {
            self->warn(emptyErrorFallback(errorText, QStringLiteral("photo_editor_set_output_size failed.")));
        }
        if (repost) {
            self->postSetOutputSizeTask(generation);
        }
    });

    if (!submit.accepted) {
        failIfCurrent(generation, submit.error.message);
    }
}

void PhotoEditorHandleActor::postSetOpcodeTask(quint64 generation)
{
    if (m_executor == nullptr) {
        failIfCurrent(generation, QStringLiteral("Photo editor actor requires a runtime executor."));
        return;
    }

    const auto weakActor = weak_from_this();
    const SubmitResult submit = m_executor->post([weakActor, generation](IRuntime *runtime) {
        const auto self = weakActor.lock();
        if (!self) {
            return;
        }

        void *handle = nullptr;
        ImageEffectParameters parameters;
        {
            QMutexLocker locker(&self->m_mutex);
            if (!self->isCurrentLocked(generation) || !self->canUseHandleLocked() || !self->m_hasOpcode) {
                self->m_opcodeTaskPending = false;
                return;
            }
            handle = self->m_handle;
            parameters = self->m_latestParameters;
        }

        RuntimeDiagnostics::logInfo(kLogScope,
                                    QStringLiteral("[GL] actor#%1 gen=%2 photo_editor_set_opcode")
                                        .arg(self->m_actorId)
                                        .arg(generation));

        QString errorText;
        RuntimeScope scope(runtime, &errorText);
        const bool ok = scope.ok() && photo_editor_set_opcode(handle, parameters, &errorText);

        bool repost = false;
        {
            QMutexLocker locker(&self->m_mutex);
            if (self->isCurrentLocked(generation)) {
                if (!ok) {
                    self->m_state = State::Failed;
                }
                self->m_opcodeTaskPending = false;
                if (ok && self->canUseHandleLocked() && self->m_hasOpcode) {
                    const ImageEffectParameters latest = self->m_latestParameters;
                    repost = latest.brightness != parameters.brightness
                        || latest.contrast != parameters.contrast
                        || latest.zoom != parameters.zoom
                        || latest.panX != parameters.panX
                        || latest.panY != parameters.panY
                        || latest.rotationDegrees != parameters.rotationDegrees
                        || latest.heavyGpuPassCount != parameters.heavyGpuPassCount
                        || latest.flipHorizontal != parameters.flipHorizontal
                        || latest.flipVertical != parameters.flipVertical;
                    if (repost) {
                        self->m_opcodeTaskPending = true;
                    }
                }
            }
        }

        if (!ok) {
            self->warn(emptyErrorFallback(errorText, QStringLiteral("photo_editor_set_opcode failed.")));
        }
        if (repost) {
            self->postSetOpcodeTask(generation);
        }
    });

    if (!submit.accepted) {
        failIfCurrent(generation, submit.error.message);
    }
}

void PhotoEditorHandleActor::postProcessTask(quint64 generation)
{
    if (m_executor == nullptr) {
        failIfCurrent(generation, QStringLiteral("Photo editor actor requires a runtime executor."));
        return;
    }

    const auto weakActor = weak_from_this();
    const SubmitResult submit = m_executor->post([weakActor, generation](IRuntime *runtime) {
        const auto self = weakActor.lock();
        if (!self) {
            return;
        }

        void *handle = nullptr;
        {
            QMutexLocker locker(&self->m_mutex);
            if (!self->isCurrentLocked(generation) || self->m_state != State::Processing || self->m_handle == nullptr) {
                self->m_processTaskPending = false;
                return;
            }

            handle = self->m_handle;
            self->m_activeProcessGeneration = generation;
        }

        RuntimeDiagnostics::logInfo(kLogScope,
                                    QStringLiteral("[GL] actor#%1 gen=%2 photo_editor_process")
                                        .arg(self->m_actorId)
                                        .arg(generation));

        QString errorText;
        RuntimeScope scope(runtime, &errorText);
        const bool ok = scope.ok()
            && photo_editor_process(handle, self.get(), &PhotoEditorHandleActor::onPhotoEditorProgress, self.get(), &errorText);

        {
            QMutexLocker locker(&self->m_mutex);
            if (self->isCurrentLocked(generation)) {
                self->m_processTaskPending = false;
                if (!ok) {
                    self->m_state = State::Failed;
                    self->m_activeProcessGeneration = 0;
                }
            }
        }

        if (!ok) {
            self->warn(emptyErrorFallback(errorText, QStringLiteral("photo_editor_process failed.")));
        }
    });

    if (!submit.accepted) {
        failIfCurrent(generation, submit.error.message);
    }
}

void PhotoEditorHandleActor::postRenderTask(quint64 generation)
{
    if (m_executor == nullptr) {
        failIfCurrent(generation, QStringLiteral("Photo editor actor requires a runtime executor."));
        return;
    }

    const auto weakActor = weak_from_this();
    const SubmitResult submit = m_executor->post([weakActor, generation](IRuntime *runtime) {
        const auto self = weakActor.lock();
        if (!self) {
            return;
        }

        void *handle = nullptr;
        QString sourceKey;
        quint64 sourceImageCacheKey = 0;
        {
            QMutexLocker locker(&self->m_mutex);
            if (!self->isCurrentLocked(generation) || self->m_state != State::Rendering || self->m_handle == nullptr) {
                return;
            }
            handle = self->m_handle;
            sourceKey = self->m_sourceKey;
            sourceImageCacheKey = self->m_sourceImageCacheKey;
        }

        RuntimeDiagnostics::logInfo(kLogScope,
                                    QStringLiteral("[GL] actor#%1 gen=%2 photo_editor_render")
                                        .arg(self->m_actorId)
                                        .arg(generation));

        QString errorText;
        RuntimeScope scope(runtime, &errorText);
        RawGpuTextureResult result;
        const bool ok = scope.ok() && photo_editor_render(handle, &result.textureId, &result.size, &errorText);
        result.metadata.insert(QStringLiteral("actorId"), qulonglong(self->m_actorId));
        result.metadata.insert(QStringLiteral("generation"), qulonglong(generation));
        result.metadata.insert(QStringLiteral("sourceKey"), sourceKey);
        result.metadata.insert(QStringLiteral("sourceImageCacheKey"), qulonglong(sourceImageCacheKey));

        bool processAgain = false;
        {
            QMutexLocker locker(&self->m_mutex);
            if (self->isCurrentLocked(generation)) {
                if (ok) {
                    self->m_state = State::Rendered;
                    processAgain = self->m_processAgainRequested;
                    self->m_processAgainRequested = false;
                } else {
                    self->m_state = State::Failed;
                }
            } else {
                RuntimeDiagnostics::logDiag(kLogScope,
                                            QStringLiteral("[GL] actor#%1 gen=%2 drop stale render result")
                                                .arg(self->m_actorId)
                                                .arg(generation));
            }
        }

        if (ok) {
            emit self->renderResult(result);
        } else {
            self->warn(emptyErrorFallback(errorText, QStringLiteral("photo_editor_render failed.")));
        }
        if (processAgain) {
            self->process();
        }
    });

    if (!submit.accepted) {
        failIfCurrent(generation, submit.error.message);
    }
}

void PhotoEditorHandleActor::postDestroyTask(void *handle, quint64 generation)
{
    if (handle == nullptr) {
        return;
    }
    if (m_executor == nullptr) {
        warn(QStringLiteral("Photo editor actor cannot destroy a handle without a runtime executor."));
        return;
    }

    const auto weakActor = weak_from_this();
    const quint64 actorId = m_actorId;
    const SubmitResult submit = m_executor->post([weakActor, handle, actorId, generation](IRuntime *runtime) {
        const auto self = weakActor.lock();

        RuntimeDiagnostics::logInfo(kLogScope,
                                    QStringLiteral("[GL] actor#%1 gen=%2 photo_editor_destroy")
                                        .arg(actorId)
                                        .arg(generation));

        QString errorText;
        RuntimeScope scope(runtime, &errorText);
        if (!scope.ok()) {
            if (self) {
                {
                    QMutexLocker locker(&self->m_mutex);
                    if (self->isCurrentLocked(generation) || self->m_state == State::Destroying) {
                        self->m_handle = handle;
                        self->m_state = State::Failed;
                    }
                }
                self->warn(emptyErrorFallback(errorText, QStringLiteral("photo_editor_destroy failed to enter runtime.")));
            }
            return;
        }
        photo_editor_destroy(handle);

        if (!self) {
            return;
        }

        {
            QMutexLocker locker(&self->m_mutex);
            if (self->isCurrentLocked(generation) || self->m_state == State::Destroying) {
                self->m_handle = nullptr;
                self->m_state = State::Destroyed;
                self->m_activeProcessGeneration = 0;
            }
        }
    });

    if (!submit.accepted) {
        warn(emptyErrorFallback(submit.error.message, QStringLiteral("Failed to post photo_editor_destroy task.")));
    }
}

void PhotoEditorHandleActor::onProcessCompleted(quint64 generation, int progress)
{
    bool shouldRender = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!isCurrentLocked(generation) || m_state != State::Processing || m_destroyRequested) {
            RuntimeDiagnostics::logDiag(kLogScope,
                                        QStringLiteral("[SDK] actor#%1 gen=%2 drop process callback progress=%3 state=%4")
                                            .arg(m_actorId)
                                            .arg(generation)
                                            .arg(progress)
                                            .arg(stateName(m_state)));
            return;
        }

        m_state = State::Processed;
        m_activeProcessGeneration = 0;
        shouldRender = true;
    }

    if (shouldRender) {
        render();
    }
}

void PhotoEditorHandleActor::failIfCurrent(quint64 generation, const QString &message)
{
    {
        QMutexLocker locker(&m_mutex);
        if (!isCurrentLocked(generation) || m_state == State::Destroying || m_state == State::Destroyed) {
            return;
        }
        m_state = State::Failed;
    }
    warn(message);
}

void PhotoEditorHandleActor::warn(const QString &message)
{
    if (message.isEmpty()) {
        return;
    }
    RuntimeDiagnostics::logWarning(kLogScope,
                                   QStringLiteral("actor#%1 %2").arg(m_actorId).arg(message));
    emit warning(message);
}

void PhotoEditorHandleActor::handlePhotoEditorProgress(int progress, bool isEnd)
{
    quint64 actorId = 0;
    quint64 generation = 0;
    State state = State::Empty;
    bool shouldComplete = false;
    {
        QMutexLocker locker(&m_mutex);
        actorId = m_actorId;
        generation = m_activeProcessGeneration;
        state = m_state;
        shouldComplete = isEnd
            && generation != 0
            && isCurrentLocked(generation)
            && state == State::Processing
            && !m_destroyRequested;
    }

    RuntimeDiagnostics::logInfo(kLogScope,
                                QStringLiteral("[SDK] actor#%1 gen=%2 progress=%3 isEnd=%4")
                                    .arg(actorId)
                                    .arg(generation)
                                    .arg(progress)
                                    .arg(isEnd));

    if (isEnd && !shouldComplete) {
        RuntimeDiagnostics::logDiag(kLogScope,
                                    QStringLiteral("[SDK] actor#%1 gen=%2 drop process callback progress=%3 state=%4")
                                        .arg(actorId)
                                        .arg(generation)
                                        .arg(progress)
                                        .arg(stateName(state)));
        return;
    }

    if (shouldComplete) {
        onProcessCompleted(generation, progress);
    }
}

bool PhotoEditorHandleActor::isCurrentLocked(quint64 generation) const
{
    return generation == m_generation;
}

bool PhotoEditorHandleActor::canUseHandleLocked() const
{
    return m_handle != nullptr
        && !m_destroyRequested
        && m_state != State::Empty
        && m_state != State::Creating
        && m_state != State::Destroying
        && m_state != State::Destroyed
        && m_state != State::Failed;
}

void PhotoEditorHandleActor::onPhotoEditorProgress(int progress, bool isEnd, void *userData)
{
    auto *actor = static_cast<PhotoEditorHandleActor *>(userData);
    if (actor == nullptr) {
        return;
    }

    actor->handlePhotoEditorProgress(progress, isEnd);
}

QString PhotoEditorHandleActor::stateName(State state)
{
    switch (state) {
    case State::Empty:
        return QStringLiteral("Empty");
    case State::Creating:
        return QStringLiteral("Creating");
    case State::Ready:
        return QStringLiteral("Ready");
    case State::Processing:
        return QStringLiteral("Processing");
    case State::Processed:
        return QStringLiteral("Processed");
    case State::Rendering:
        return QStringLiteral("Rendering");
    case State::Rendered:
        return QStringLiteral("Rendered");
    case State::Destroying:
        return QStringLiteral("Destroying");
    case State::Destroyed:
        return QStringLiteral("Destroyed");
    case State::Failed:
        return QStringLiteral("Failed");
    }

    return QStringLiteral("Unknown");
}
