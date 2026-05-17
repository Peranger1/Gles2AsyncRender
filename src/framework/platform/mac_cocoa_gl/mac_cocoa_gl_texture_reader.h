#pragma once

#include "framework/platform/reader.h"

#include <memory>
#include <QHash>
#include <vector>

class QOpenGLContext;
class QOpenGLFunctions;
class PlatformPresentationEvents;
class MacIoSurfaceTextureSlots;
struct MacIoSurfaceSlotInfo;

class MacCocoaGlTextureReader final : public IReader
{
public:
    MacCocoaGlTextureReader(const std::shared_ptr<MacIoSurfaceTextureSlots> &slotPool,
                            PlatformPresentationEvents *presentationEvents);
    ~MacCocoaGlTextureReader() override;

    bool attachToCurrentContext(QString *error) override;
    bool acquire(const TextureTicket &ticket, TextureLease *lease, QString *error) override;
    void release(const TextureLease &lease) override;
    void detach() override;

private:
    struct ImportedSlot;

#if defined(Q_OS_MACOS)
    bool ensureImportedSlot(int slotIndex, const MacIoSurfaceSlotInfo &slotInfo, QString *error);
    void destroyImportedSlot(int slotIndex);
#endif

    std::shared_ptr<MacIoSurfaceTextureSlots> m_slotPool;
    PlatformPresentationEvents *m_presentationEvents = nullptr;
    QOpenGLContext *m_context = nullptr;
    QOpenGLFunctions *m_gl = nullptr;
#if defined(Q_OS_MACOS)
    std::vector<std::unique_ptr<ImportedSlot>> m_importedSlots;
    QHash<GLuint, int> m_slotIndexByTextureId;
#endif
};
