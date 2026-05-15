#pragma once

#include "framework/core/work_processor.h"

class PhotoEditorCpuPreviewProcessor final : public IWorkProcessor
{
public:
    bool initialize(IWorkRuntime &runtime, QString *error) override;
    void setWakeCallback(ProcessorWakeCallback callback) override;
    bool start(const WorkEnvelope &work,
               IWorkObserver *observer,
               QString *error) override;
    bool isOutputReady() const override;
    bool collectOutputIfReady(ProcessorOutput *output, QString *error) override;
    void shutdown() override;

private:
    ProcessorWakeCallback m_wakeCallback;
    bool m_outputReady = false;
    bool m_hasOutput = false;
    ProcessorOutput m_output;
};
