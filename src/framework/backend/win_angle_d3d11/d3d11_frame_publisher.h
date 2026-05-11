#pragma once

#include "framework/core/frame_publisher.h"

#include <QSize>
#include <QString>

#include <QtANGLE/GLES2/gl2.h>

#include <memory>

class AngleStandaloneRuntime;
class ISharedFrameSlotPool;

class D3D11FramePublisher final : public IFramePublisher
{
public:
    D3D11FramePublisher();
    ~D3D11FramePublisher();

    bool initialize(AngleStandaloneRuntime *runtime,
                    ISharedFrameSlotPool *slotPool,
                    QString *error);

    bool publishToSlot(GLuint sourceTextureId,
                       const QSize &sourceSize,
                       int slotIndex,
                       QString *error) override;

    void releaseGlResources() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
