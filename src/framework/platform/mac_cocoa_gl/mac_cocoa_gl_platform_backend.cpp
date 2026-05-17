#include "mac_cocoa_gl_platform_backend.h"

#include "mac_iosurface_texture_slots.h"
#include "mac_cocoa_gl_runtime.h"
#include "mac_cocoa_gl_shared_state.h"
#include "mac_cocoa_gl_texture_reader.h"
#include "mac_cocoa_gl_texture_writer.h"

#include <QOffscreenSurface>

MacCocoaGlPlatformBackend::MacCocoaGlPlatformBackend(int slotCount)
    : m_slotCount(slotCount)
    , m_presentationEvents(std::make_unique<PlatformPresentationEvents>())
    , m_sharedState(std::make_shared<MacCocoaGlSharedState>())
    , m_slotPool(std::make_shared<MacIoSurfaceTextureSlots>(slotCount))
    , m_reader(std::make_unique<MacCocoaGlTextureReader>(m_slotPool, m_presentationEvents.get()))
    , m_writer(std::make_unique<MacCocoaGlTextureWriter>(m_slotPool))
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

MacCocoaGlPlatformBackend::~MacCocoaGlPlatformBackend() = default;

bool MacCocoaGlPlatformBackend::preparePresentationContext(const PresentationContext &context, QString *error)
{
    Q_UNUSED(m_slotCount);
    if (context.format.renderableType() == QSurfaceFormat::DefaultRenderableType) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlPlatformBackend requires a valid presentation surface format.");
        }
        return false;
    }

    auto offscreenSurface = std::make_unique<QOffscreenSurface>();
    offscreenSurface->setFormat(context.format);
    if (context.screen) {
        offscreenSurface->setScreen(context.screen);
    }
    offscreenSurface->create();
    if (!offscreenSurface->isValid()) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlPlatformBackend failed to create a compatible QOffscreenSurface.");
        }
        return false;
    }

    m_sharedState->screen = context.screen;
    m_sharedState->format = context.format;
    m_sharedState->offscreenSurface = std::move(offscreenSurface);
    m_sharedState->slotPool = m_slotPool;
    return true;
}

std::unique_ptr<IRuntime> MacCocoaGlPlatformBackend::createRuntime() const
{
    return std::make_unique<MacCocoaGlRuntime>(m_sharedState);
}

IReader *MacCocoaGlPlatformBackend::reader() const
{
    return m_reader.get();
}

IWriter *MacCocoaGlPlatformBackend::writer() const
{
    return m_writer.get();
}

PlatformPresentationEvents *MacCocoaGlPlatformBackend::presentationEvents() const
{
    return m_presentationEvents.get();
}
