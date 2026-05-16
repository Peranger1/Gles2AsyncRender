#pragma once

#include "framework/platform/writer.h"

#include <memory>

class D3D11SharedTextureSlots;
class WinAngleRuntime;

class WinAngleTextureWriter final : public IWriter
{
public:
    struct RuntimeBindingState final
    {
        WinAngleRuntime *runtime = nullptr;
    };

    WinAngleTextureWriter(const std::shared_ptr<D3D11SharedTextureSlots> &slotPool,
                          const std::shared_ptr<RuntimeBindingState> &runtimeBinding);
    ~WinAngleTextureWriter() override;

    bool publishTexture(GLuint sourceTextureId,
                        const QSize &size,
                        TextureTicket *ticket,
                        QString *error) override;
    void reset() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
