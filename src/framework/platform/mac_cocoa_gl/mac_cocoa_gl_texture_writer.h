#pragma once

#include "framework/platform/writer.h"

#include <memory>

class MacIoSurfaceTextureSlots;
class RuntimeHost;
class IRuntime;
class IWriterEvents;

class MacCocoaGlTextureWriter final : public IWriter
{
public:
    explicit MacCocoaGlTextureWriter(const std::shared_ptr<MacIoSurfaceTextureSlots> &slotPool);
    ~MacCocoaGlTextureWriter() override;

    void attach(RuntimeHost *host, IRuntime *runtime, IWriterEvents *events) override;
    bool submitTexture(GLuint sourceTextureId,
                       const QSize &size,
                       quint64 outputRevision,
                       QString *error) override;
    void notifyPresentationCapacityAvailable() override;
    void reset() override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
