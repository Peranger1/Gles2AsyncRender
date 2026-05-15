#pragma once

#include "framework/core/work_processor.h"

#include <QObject>

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
    bool isOutputReady() const override;
    bool collectOutputIfReady(ProcessorOutput *output, QString *error) override;
    void shutdown() override;

private slots:
    void onProgressEvent(int progress, bool isEnd);

private:
    struct ActiveExecution final
    {
        RequestId requestId = 0;
        QString outputKind;
        IWorkObserver *observer = nullptr;
        bool outputReady = false;
    };

    static void processProgressThunk(int progress, bool isEnd, void *userData);
    void clearActiveExecution();
    void invokeWakeCallback();

    std::unique_ptr<PhotoEditorRenderSession> m_session;
    AngleStandaloneRuntime *m_runtime = nullptr;
    ProcessorWakeCallback m_wakeCallback;
    ActiveExecution m_activeExecution;
    bool m_hasActiveExecution = false;
};
