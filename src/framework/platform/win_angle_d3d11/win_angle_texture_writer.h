#pragma once

#include "framework/platform/writer.h"

#include <memory>

class D3D11SharedTextureSlots;

class WinAngleTextureWriter final : public IWriter
{
public:
    explicit WinAngleTextureWriter(const std::shared_ptr<D3D11SharedTextureSlots> &slotPool);
    ~WinAngleTextureWriter() override;

    void attach(RuntimeHost *host, IRuntime *runtime, IWriterEvents *events) override;
    bool submitTexture(GLuint sourceTextureId,
                       const QSize &size,
                       quint64 outputRevision,
                       QString *error) override;
    void notifyPresentationCapacityAvailable() override;
    void reset() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
