#include "photo_editor_work_processor.h"

#include "framework/backend/win_angle_d3d11/angle_standalone_runtime.h"
#include "photo_editor_render_session.h"
#include "photo_editor_render_payload.h"
#include "framework/core/artifact_builder.h"

#include <QMetaObject>

namespace
{
QSize safeOutputSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}
}

PhotoEditorWorkProcessor::PhotoEditorWorkProcessor(QObject *parent)
    : QObject(parent)
    , m_session(std::make_unique<PhotoEditorRenderSession>())
{
}

PhotoEditorWorkProcessor::~PhotoEditorWorkProcessor()
{
    shutdown();
}

bool PhotoEditorWorkProcessor::initialize(IWorkRuntime &runtime, QString *error)
{
    m_workRuntime = &runtime;
    m_runtime = dynamic_cast<AngleStandaloneRuntime *>(&runtime);
    if (m_runtime == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorWorkProcessor requires an AngleStandaloneRuntime.");
        }
        return false;
    }

    return m_session && m_session->initialize(m_runtime, error);
}

void PhotoEditorWorkProcessor::setWakeCallback(ProcessorWakeCallback callback)
{
    m_wakeCallback = std::move(callback);
}

bool PhotoEditorWorkProcessor::start(const WorkEnvelope &work,
                                     IWorkObserver *observer,
                                     QString *error)
{
    if (!m_session || m_runtime == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorWorkProcessor is not initialized.");
        }
        return false;
    }
    if (m_hasActiveExecution) {
        if (error) {
            *error = QStringLiteral("PhotoEditorWorkProcessor does not support concurrent work.");
        }
        return false;
    }

    const auto payload = std::static_pointer_cast<PhotoEditorRenderPayload>(work.payload);
    if (!payload || payload->sourceImage.isNull()) {
        if (error) {
            *error = QStringLiteral("PhotoEditor work is missing a valid payload.");
        }
        return false;
    }

    if (observer) {
        observer->onStateChanged(work.workId, WorkState::Admitted);
        observer->onStateChanged(work.workId, WorkState::Executing);
    }

    PhotoEditorRequest request;
    request.sequence = work.workId;
    const QSize hintedOutputSize = work.hints.value(QStringLiteral("outputSize")).toSize();
    request.outputSize = hintedOutputSize.isValid()
        ? safeOutputSize(hintedOutputSize)
        : safeOutputSize(payload->sourceImage.size());
    request.payload = payload;

    m_cancelRequested = false;
    m_activeExecution = {};
    m_activeExecution.workId = work.workId;
    m_activeExecution.sequence = request.sequence;
    m_activeExecution.observer = observer;
    m_activeExecution.artifactMetadata.insert(QStringLiteral("sourceKey"), payload->sourceKey);
    m_activeExecution.artifactMetadata.insert(QStringLiteral("sourceImageCacheKey"), qulonglong(payload->sourceImageCacheKey));
    m_activeExecution.artifactMetadata.insert(QStringLiteral("workflowKey"), work.workflowKey);
    m_activeExecution.artifactMetadata.insert(QStringLiteral("streamKey"), work.streamKey);
    m_activeExecution.artifactMetadata.insert(QStringLiteral("workId"), qulonglong(work.workId));
    m_hasActiveExecution = true;

    const bool submitOk = m_session->submitRequest(request,
                                                   this,
                                                   &PhotoEditorWorkProcessor::processProgressThunk,
                                                   this,
                                                   error);
    if (!submitOk) {
        clearActiveExecution();
        return false;
    }

    return true;
}

bool PhotoEditorWorkProcessor::isArtifactReady() const
{
    return m_hasActiveExecution && m_activeExecution.artifactReady;
}

bool PhotoEditorWorkProcessor::collectIfReady(IArtifactBuilder &builder, QString *error)
{
    if (!m_hasActiveExecution) {
        if (error) {
            *error = QStringLiteral("PhotoEditorWorkProcessor has no active work.");
        }
        return false;
    }
    if (m_cancelRequested) {
        if (m_activeExecution.observer) {
            m_activeExecution.observer->onStateChanged(m_activeExecution.workId, WorkState::Cancelled);
        }
        if (error) {
            *error = QStringLiteral("PhotoEditor work was cancelled.");
        }
        clearActiveExecution();
        return false;
    }
    if (!m_activeExecution.artifactReady) {
        if (error) {
            error->clear();
        }
        return false;
    }

    if (m_activeExecution.observer) {
        m_activeExecution.observer->onStateChanged(m_activeExecution.workId, WorkState::ProducingArtifact);
    }

    PhotoEditorRenderedTexture output;
    if (!m_session->renderReadyTexture(&output, error)) {
        clearActiveExecution();
        return false;
    }

    QMap<QString, QVariant> metadata = m_activeExecution.artifactMetadata;
    metadata.insert(QStringLiteral("workSequence"), qulonglong(m_activeExecution.sequence));
    const bool buildOk = builder.setTexture(output.textureId, output.size, metadata, error);
    clearActiveExecution();
    return buildOk;
}

void PhotoEditorWorkProcessor::cancel(quint64 workId)
{
    if (m_hasActiveExecution && m_activeExecution.workId == workId) {
        m_cancelRequested = true;
        invokeWakeCallback();
    }
}

void PhotoEditorWorkProcessor::shutdown()
{
    m_cancelRequested = true;
    clearActiveExecution();
    if (m_session) {
        m_session->shutdown();
    }
    m_runtime = nullptr;
    m_workRuntime = nullptr;
    m_wakeCallback = {};
}

void PhotoEditorWorkProcessor::onProgressEvent(int progress, bool isEnd)
{
    if (!m_session) {
        return;
    }

    m_session->handleProgressEvent(progress, isEnd);
    if (m_hasActiveExecution && m_activeExecution.observer != nullptr) {
        m_activeExecution.observer->onProgress(m_activeExecution.workId, progress, isEnd);
    }

    if (m_hasActiveExecution && isEnd) {
        m_activeExecution.artifactReady = true;
        invokeWakeCallback();
    }
}

void PhotoEditorWorkProcessor::processProgressThunk(int progress, bool isEnd, void *userData)
{
    auto *processor = static_cast<PhotoEditorWorkProcessor *>(userData);
    if (processor == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(processor,
                              "onProgressEvent",
                              Qt::QueuedConnection,
                              Q_ARG(int, progress),
                              Q_ARG(bool, isEnd));
}

void PhotoEditorWorkProcessor::clearActiveExecution()
{
    m_activeExecution = {};
    m_hasActiveExecution = false;
    m_cancelRequested = false;
}

void PhotoEditorWorkProcessor::invokeWakeCallback()
{
    if (m_wakeCallback) {
        m_wakeCallback();
    }
}
