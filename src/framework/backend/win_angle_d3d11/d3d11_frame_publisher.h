#pragma once

#include "framework/core/frame_publisher.h"

#include <QSize>
#include <QString>

#include <QtANGLE/GLES2/gl2.h>

#include <memory>

class AngleStandaloneRuntime;
class IFrameWriter;

class D3D11FramePublisher final : public IFramePublisher
{
public:
    D3D11FramePublisher();
    ~D3D11FramePublisher();

    bool initialize(IWorkRuntime &runtime,
                    IFrameWriter &frameWriter,
                    QString *error) override;
    bool publish(const WorkEnvelope &work,
                 const GpuTextureResult &gpuResult,
                 FrameTicket *ticket,
                 QString *error) override;
    void shutdown() override;

private:
    bool publishToSlot(GLuint sourceTextureId,
                       const QSize &sourceSize,
                       int slotIndex,
                       QString *error);
    void releaseGlResources();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    AngleStandaloneRuntime *m_runtime = nullptr;
    IFrameWriter *m_frameWriter = nullptr;
    quint64 m_publicationCounter = 0;
};
