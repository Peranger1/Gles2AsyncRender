#pragma once

#include "framework/platform/platform_backend.h"
#include "win_angle_texture_writer.h"

#include <memory>

class D3D11SharedTextureSlots;
class WinAngleTextureReader;

class WinAnglePlatformBackend final : public IPlatformBackend
{
public:
    explicit WinAnglePlatformBackend(int slotCount = 3);
    ~WinAnglePlatformBackend() override;

    std::unique_ptr<IRuntime> createRuntime() const override;
    IReader *reader() const override;
    IWriter *writer() const override;

private:
    std::shared_ptr<WinAngleTextureWriter::RuntimeBindingState> m_runtimeBinding;
    std::shared_ptr<D3D11SharedTextureSlots> m_slotPool;
    std::unique_ptr<WinAngleTextureReader> m_reader;
    std::unique_ptr<WinAngleTextureWriter> m_writer;
};
