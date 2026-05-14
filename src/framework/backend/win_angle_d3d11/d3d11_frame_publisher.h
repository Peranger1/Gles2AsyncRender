#pragma once

#include "framework/core/artifact_publisher.h"

#include <QSize>
#include <QString>

#include <QtANGLE/GLES2/gl2.h>

#include <memory>

class AngleStandaloneRuntime;
class ISharedFrameSlotPool;

class D3D11FramePublisher final : public IArtifactPublisher
{
public:
    D3D11FramePublisher();
    ~D3D11FramePublisher();

    bool initialize(IWorkRuntime &runtime, QString *error) override;
    bool publish(const WorkEnvelope &work,
                 const ArtifactSnapshot &artifact,
                 PublicationTicket *ticket,
                 QString *error) override;
    void shutdown() override;

    void setSlotPool(ISharedFrameSlotPool *slotPool);

private:
    bool publishToSlot(GLuint sourceTextureId,
                       const QSize &sourceSize,
                       int slotIndex,
                       QString *error);
    void releaseGlResources();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    AngleStandaloneRuntime *m_runtime = nullptr;
    ISharedFrameSlotPool *m_slotPool = nullptr;
    quint64 m_publicationCounter = 0;
};
