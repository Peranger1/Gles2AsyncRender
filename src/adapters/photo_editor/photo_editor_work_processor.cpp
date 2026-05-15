#include "photo_editor_work_processor.h"

#include "framework/backend/win_angle_d3d11/angle_standalone_runtime.h"
#include "photo_editor_render_payload.h"
#include "photo_editor_render_session.h"

#include <QMetaObject>

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

    const auto payload = std::dynamic_pointer_cast<PhotoEditorRenderPayload>(work.payload);
    if (!payload || payload->sourceImage.isNull()) {
        if (error) {
            *error = QStringLiteral("PhotoEditor work is missing a valid payload.");
        }
        return false;
    }

    if (observer) {
        observer->onStateChanged(work.requestId, WorkState::Admitted);
        observer->onStateChanged(work.requestId, WorkState::Executing);
    }

    m_activeExecution = {};
    m_activeExecution.requestId = work.requestId;
    m_activeExecution.outputKind = QStringLiteral("photo_editor.preview_frame");
    m_activeExecution.observer = observer;
    m_hasActiveExecution = true;

    const bool submitOk = m_session->submitRequest(payload,
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

bool PhotoEditorWorkProcessor::isOutputReady() const
{
    return m_hasActiveExecution && m_activeExecution.outputReady;
}

bool PhotoEditorWorkProcessor::collectOutputIfReady(ProcessorOutput *output, QString *error)
{
    if (output == nullptr) {
        if (error) {
            *error = QStringLiteral("PhotoEditorWorkProcessor requires a valid output target.");
        }
        return false;
    }
    if (!m_hasActiveExecution) {
        if (error) {
            *error = QStringLiteral("PhotoEditorWorkProcessor has no active work.");
        }
        return false;
    }
    if (!m_activeExecution.outputReady) {
        if (error) {
            error->clear();
        }
        return false;
    }

    if (m_activeExecution.observer) {
        m_activeExecution.observer->onStateChanged(m_activeExecution.requestId, WorkState::ProducingOutput);
    }

    GLuint renderedTextureId = 0U;
    QSize renderedTextureSize;
    if (!m_session->renderReadyTexture(&renderedTextureId, &renderedTextureSize, error)) {
        clearActiveExecution();
        return false;
    }

    ProcessorOutput processorOutput;
    processorOutput.requestId = m_activeExecution.requestId;
    processorOutput.outputKind = m_activeExecution.outputKind;
    GpuTextureResult textureResult;
    textureResult.textureId = renderedTextureId;
    textureResult.size = renderedTextureSize;
    processorOutput.payload = textureResult;
    clearActiveExecution();
    *output = std::move(processorOutput);
    return true;
}

void PhotoEditorWorkProcessor::shutdown()
{
    clearActiveExecution();
    if (m_session) {
        m_session->shutdown();
    }
    m_runtime = nullptr;
    m_wakeCallback = {};
}

void PhotoEditorWorkProcessor::onProgressEvent(int progress, bool isEnd)
{
    if (!m_session) {
        return;
    }

    m_session->handleProgressEvent(progress, isEnd);
    if (m_hasActiveExecution && m_activeExecution.observer != nullptr) {
        m_activeExecution.observer->onProgress(m_activeExecution.requestId, progress, isEnd);
    }

    if (m_hasActiveExecution && isEnd) {
        m_activeExecution.outputReady = true;
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
}

void PhotoEditorWorkProcessor::invokeWakeCallback()
{
    if (m_wakeCallback) {
        m_wakeCallback();
    }
}
