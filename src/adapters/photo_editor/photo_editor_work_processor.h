#pragma once

#include "framework/core/work_processor.h"

#include <QObject>
#include <QPointer>

#include <memory>

class AngleStandaloneRuntime;
class PhotoEditorRenderSession;

class PhotoEditorWorkProcessor final : public QObject, public IWorkProcessor
{
    Q_OBJECT

public:
    explicit PhotoEditorWorkProcessor(QObject *parent = nullptr);
    ~PhotoEditorWorkProcessor() override;

    bool initialize(IWorkRuntime &runtime, QString *error) override;
    void setWakeCallback(ProcessorWakeCallback callback) override;
    bool start(const WorkEnvelope &work,
               IWorkObserver *observer,
               QString *error) override;
    bool isArtifactReady() const override;
    bool collectIfReady(IArtifactBuilder &builder, QString *error) override;
    void cancel(quint64 workId) override;
    void shutdown() override;

private slots:
    void onProgressEvent(int progress, bool isEnd);

private:
    struct ActiveExecution final
    {
        quint64 workId = 0;
        quint64 sequence = 0;
        IWorkObserver *observer = nullptr;
        QMap<QString, QVariant> artifactMetadata;
        bool artifactReady = false;
    };

    static void processProgressThunk(int progress, bool isEnd, void *userData);
    void clearActiveExecution();
    void invokeWakeCallback();

    std::unique_ptr<PhotoEditorRenderSession> m_session;
    AngleStandaloneRuntime *m_runtime = nullptr;
    IWorkRuntime *m_workRuntime = nullptr;
    ProcessorWakeCallback m_wakeCallback;
    ActiveExecution m_activeExecution;
    bool m_hasActiveExecution = false;
    bool m_cancelRequested = false;
};
