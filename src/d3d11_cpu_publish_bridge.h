#pragma once

#include <QSize>
#include <QString>

#include <QtANGLE/GLES2/gl2.h>

#include <memory>

class AngleStandaloneRuntime;
class D3D11NativeSlotPool;

class D3D11CpuPublishBridge final
{
public:
    D3D11CpuPublishBridge();
    ~D3D11CpuPublishBridge();

    bool initialize(AngleStandaloneRuntime *runtime,
                    D3D11NativeSlotPool *slotPool,
                    QString *error);

    bool publishToSlot(GLuint sourceTextureId,
                       const QSize &sourceSize,
                       int slotIndex,
                       QString *error);

    void releaseGlResources();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
