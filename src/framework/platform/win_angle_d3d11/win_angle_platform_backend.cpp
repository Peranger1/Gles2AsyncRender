#include "win_angle_platform_backend.h"

#include "d3d11_shared_texture_slots.h"
#include "win_angle_runtime.h"
#include "win_angle_texture_reader.h"
#include "win_angle_texture_writer.h"

WinAnglePlatformBackend::WinAnglePlatformBackend(int slotCount)
    : m_presentationEvents(std::make_unique<PlatformPresentationEvents>())
    , m_slotPool(std::make_shared<D3D11SharedTextureSlots>(slotCount))
    , m_writer(std::make_unique<WinAngleTextureWriter>(m_slotPool))
    , m_reader(std::make_unique<WinAngleTextureReader>(m_slotPool, m_presentationEvents.get()))
{
    QObject::connect(m_presentationEvents.get(),
                     &PlatformPresentationEvents::publishCapacityAvailable,
                     m_presentationEvents.get(),
                     [this]() {
                         if (m_writer) {
                             m_writer->notifyPresentationCapacityAvailable();
                         }
                     },
                     Qt::QueuedConnection);
}

WinAnglePlatformBackend::~WinAnglePlatformBackend() = default;

bool WinAnglePlatformBackend::preparePresentationContext(const PresentationContext &context, QString *error)
{
    Q_UNUSED(context);
    Q_UNUSED(error);
    return true;
}

std::unique_ptr<IRuntime> WinAnglePlatformBackend::createRuntime() const
{
    return std::make_unique<WinAngleRuntime>();
}

IReader *WinAnglePlatformBackend::reader() const
{
    return m_reader.get();
}

IWriter *WinAnglePlatformBackend::writer() const
{
    return m_writer.get();
}

PlatformPresentationEvents *WinAnglePlatformBackend::presentationEvents() const
{
    return m_presentationEvents.get();
}
