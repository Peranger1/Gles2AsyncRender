#pragma once

#include "framework/platform/platform_backend.h"
#include "framework/platform/presentation_events.h"

#include <memory>

class MacCocoaGlTextureReader;
class MacCocoaGlTextureWriter;
class MacIoSurfaceTextureSlots;
struct MacCocoaGlSharedState;

class MacCocoaGlPlatformBackend final : public IPlatformBackend
{
public:
    explicit MacCocoaGlPlatformBackend(int slotCount = 3);
    ~MacCocoaGlPlatformBackend() override;

    bool preparePresentationContext(const PresentationContext &context, QString *error) override;
    std::unique_ptr<IRuntime> createRuntime() const override;
    IReader *reader() const override;
    IWriter *writer() const override;
    PlatformPresentationEvents *presentationEvents() const override;

private:
    Q_DISABLE_COPY(MacCocoaGlPlatformBackend)

    int m_slotCount = 0;
    std::unique_ptr<PlatformPresentationEvents> m_presentationEvents;
    std::shared_ptr<MacCocoaGlSharedState> m_sharedState;
    std::shared_ptr<MacIoSurfaceTextureSlots> m_slotPool;
    std::unique_ptr<MacCocoaGlTextureReader> m_reader;
    std::unique_ptr<MacCocoaGlTextureWriter> m_writer;
};
