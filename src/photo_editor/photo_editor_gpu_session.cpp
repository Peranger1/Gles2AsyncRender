#include "photo_editor_gpu_session.h"

#include "photo_editor_gles2_backend.h"
#include "framework/execution/runtime_scope.h"

#include <QMetaObject>

namespace
{
thread_local IRuntime *g_photoEditorRuntime = nullptr;

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}
}

PhotoEditorGpuSession::PhotoEditorGpuSession(QObject *parent)
    : QObject(parent)
{
}

PhotoEditorGpuSession::~PhotoEditorGpuSession()
{
    if (m_runtime) {
        shutdown(m_runtime);
    }
}

ExecutionOutcome<void> PhotoEditorGpuSession::initialize(IRuntime *runtime)
{
    if (runtime == nullptr) {
        ExecutionError error;
        error.message = QStringLiteral("PhotoEditorGpuSession requires a valid runtime.");
        return ExecutionOutcome<void>::failure(std::move(error));
    }
    m_runtime = runtime;
    return ExecutionOutcome<void>::success();
}

void PhotoEditorGpuSession::shutdown(IRuntime *runtime)
{
    if (m_activeDone) {
        ExecutionError error;
        error.state = TaskState::Shutdown;
        error.message = QStringLiteral("The GPU session was shut down before the task completed.");
        auto done = std::move(m_activeDone);
        m_activeTaskId = 0;
        done(ExecutionOutcome<RawGpuTextureResult>::failure(std::move(error)));
    }

    destroySession(runtime ? runtime : m_runtime);
    m_runtime = nullptr;
    m_libraryInitialized = false;
    m_sourceKey.clear();
    m_sourceImageCacheKey = 0;
    m_sourceImage = QImage();
}

ExecutionOutcome<void> PhotoEditorGpuSession::ensureSource(IRuntime *runtime,
                                                           const PhotoEditorSourceSnapshot &source,
                                                           const QSize &outputSize)
{
    if (runtime == nullptr || source.sourceImage.isNull()) {
        ExecutionError error;
        error.message = QStringLiteral("PhotoEditorGpuSession requires a valid runtime and source image.");
        return ExecutionOutcome<void>::failure(std::move(error));
    }

    m_runtime = runtime;
    ExecutionOutcome<void> initOutcome = ensureLibraryInitialized(runtime);
    if (!initOutcome.ok()) {
        return initOutcome;
    }

    if (m_handle == nullptr || m_sourceKey != source.sourceKey || m_sourceImageCacheKey != source.sourceImageCacheKey) {
        m_sourceKey = source.sourceKey;
        m_sourceImageCacheKey = source.sourceImageCacheKey;
        m_sourceImage = source.sourceImage;
        return rebuildSession(runtime, source, outputSize);
    }

    QString errorText;
    RuntimeScope scope(runtime, &errorText);
    if (!scope.ok()) {
        ExecutionError error;
        error.message = errorText;
        return ExecutionOutcome<void>::failure(std::move(error));
    }

    if (!photo_editor_set_output_size(m_handle, sanitizedSize(outputSize), &errorText)) {
        ExecutionError error;
        error.message = errorText;
        return ExecutionOutcome<void>::failure(std::move(error));
    }

    return ExecutionOutcome<void>::success();
}

void PhotoEditorGpuSession::renderPreviewAsync(const TaskContext &context,
                                               const PhotoEditorGpuPreviewArgs &args,
                                               std::function<void(ExecutionOutcome<RawGpuTextureResult>)> done)
{
    if (!done) {
        return;
    }

    if (m_activeDone) {
        ExecutionError error;
        error.message = QStringLiteral("PhotoEditorGpuSession does not support concurrent preview tasks.");
        done(ExecutionOutcome<RawGpuTextureResult>::failure(std::move(error)));
        return;
    }

    ExecutionOutcome<void> initOutcome = initialize(context.runtime);
    if (!initOutcome.ok()) {
        done(ExecutionOutcome<RawGpuTextureResult>::failure(initOutcome.error()));
        return;
    }

    ExecutionOutcome<void> sourceOutcome = ensureSource(context.runtime, args.source, args.outputSize);
    if (!sourceOutcome.ok()) {
        done(ExecutionOutcome<RawGpuTextureResult>::failure(sourceOutcome.error()));
        return;
    }

    QString errorText;
    RuntimeScope scope(context.runtime, &errorText);
    if (!scope.ok()) {
        ExecutionError error;
        error.message = errorText;
        done(ExecutionOutcome<RawGpuTextureResult>::failure(std::move(error)));
        return;
    }

    bool ok = photo_editor_set_output_size(m_handle, sanitizedSize(args.outputSize), &errorText);
    if (ok) {
        ok = photo_editor_set_opcode(m_handle, args.parameters, &errorText);
    }
    if (ok) {
        ok = photo_editor_process(m_handle,
                                  this,
                                  &PhotoEditorGpuSession::processProgressThunk,
                                  this,
                                  &errorText);
    }

    if (!ok) {
        ExecutionError error;
        error.message = errorText;
        done(ExecutionOutcome<RawGpuTextureResult>::failure(std::move(error)));
        return;
    }

    m_activeTaskId = context.taskId;
    m_activeDone = std::move(done);
}

void PhotoEditorGpuSession::onProgressEvent(int progress, bool isEnd)
{
    Q_UNUSED(progress);

    if (!isEnd || !m_activeDone || m_runtime == nullptr || m_handle == nullptr) {
        return;
    }

    const ExecutionOutcome<RawGpuTextureResult> result = collectRenderResult(m_runtime);
    auto done = std::move(m_activeDone);
    m_activeTaskId = 0;
    if (done) {
        done(result);
    }
}

void PhotoEditorGpuSession::processProgressThunk(int progress, bool isEnd, void *userData)
{
    auto *session = static_cast<PhotoEditorGpuSession *>(userData);
    if (session == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(session,
                              "onProgressEvent",
                              Qt::QueuedConnection,
                              Q_ARG(int, progress),
                              Q_ARG(bool, isEnd));
}

ExecutionOutcome<void> PhotoEditorGpuSession::ensureLibraryInitialized(IRuntime *runtime)
{
    if (m_libraryInitialized) {
        return ExecutionOutcome<void>::success();
    }
    if (runtime == nullptr) {
        ExecutionError error;
        error.message = QStringLiteral("PhotoEditorGpuSession runtime is unavailable.");
        return ExecutionOutcome<void>::failure(std::move(error));
    }

    QString errorText;
    RuntimeScope scope(runtime, &errorText);
    if (!scope.ok()) {
        ExecutionError error;
        error.message = errorText;
        return ExecutionOutcome<void>::failure(std::move(error));
    }

    g_photoEditorRuntime = runtime;
    const bool ok = photo_editor_init([](const char *name) -> void * {
        return g_photoEditorRuntime ? g_photoEditorRuntime->resolveProc(name) : nullptr;
    }, &errorText);
    g_photoEditorRuntime = nullptr;

    if (!ok) {
        ExecutionError error;
        error.message = errorText;
        return ExecutionOutcome<void>::failure(std::move(error));
    }

    m_libraryInitialized = true;
    return ExecutionOutcome<void>::success();
}

ExecutionOutcome<void> PhotoEditorGpuSession::rebuildSession(IRuntime *runtime,
                                                             const PhotoEditorSourceSnapshot &source,
                                                             const QSize &outputSize)
{
    destroySession(runtime);

    QString errorText;
    RuntimeScope scope(runtime, &errorText);
    if (!scope.ok()) {
        ExecutionError error;
        error.message = errorText;
        return ExecutionOutcome<void>::failure(std::move(error));
    }

    m_handle = photo_editor_create(source.sourceImage, &errorText);
    bool ok = m_handle != nullptr;
    if (ok) {
        ok = photo_editor_set_output_size(m_handle, sanitizedSize(outputSize), &errorText);
    }
    if (!ok) {
        m_handle = nullptr;
        ExecutionError error;
        error.message = errorText.isEmpty()
            ? QStringLiteral("PhotoEditorGpuSession failed to rebuild the GPU session.")
            : errorText;
        return ExecutionOutcome<void>::failure(std::move(error));
    }

    return ExecutionOutcome<void>::success();
}

ExecutionOutcome<RawGpuTextureResult> PhotoEditorGpuSession::collectRenderResult(IRuntime *runtime)
{
    QString errorText;
    RuntimeScope scope(runtime, &errorText);
    if (!scope.ok()) {
        ExecutionError error;
        error.message = errorText;
        return ExecutionOutcome<RawGpuTextureResult>::failure(std::move(error));
    }

    GLuint textureId = 0U;
    QSize size;
    if (!photo_editor_render(m_handle, &textureId, &size, &errorText)) {
        ExecutionError error;
        error.message = errorText;
        return ExecutionOutcome<RawGpuTextureResult>::failure(std::move(error));
    }

    RawGpuTextureResult gpuResult;
    gpuResult.textureId = textureId;
    gpuResult.size = size;
    return ExecutionOutcome<RawGpuTextureResult>::success(std::move(gpuResult));
}

void PhotoEditorGpuSession::destroySession(IRuntime *runtime)
{
    if (m_handle == nullptr) {
        return;
    }

    QString errorText;
    RuntimeScope scope(runtime, &errorText);
    Q_UNUSED(errorText);
    photo_editor_destroy(m_handle);
    m_handle = nullptr;
}
