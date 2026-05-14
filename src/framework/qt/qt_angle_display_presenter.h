#pragma once

#include "src/framework/core/artifact_presenter.h"
#include "src/framework/core/gl_presentation_target.h"
#include "src/framework/core/shared_frame_slot_pool.h"

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <memory>

#include <QtANGLE/EGL/egl.h>

#include <wrl/client.h>

struct IDXGIKeyedMutex;

namespace QtAngleEglTools
{
struct ResolvedEglApi;
}

class QtAngleDisplayPresenter final : public IArtifactPresenter
{
public:
    QtAngleDisplayPresenter();
    ~QtAngleDisplayPresenter();

    void setSlotPool(const std::shared_ptr<ISharedFrameSlotPool> &slotPool);
    QString lastRuntimeLog() const;
    bool initialize(IPresentationTarget &target, QString *error) override;
    bool enqueue(const PublicationTicket &ticket, QString *error) override;
    bool present(PresentationFeedback *feedback, QString *error) override;
    void shutdown() override;
    void onOutputSizeChanged(const QSize &size);

private:
    struct ImportedSlot final
    {
        quint64 generation = 0;
        quintptr sharedHandle = 0;
        QSize size;
        EGLSurface surface = EGL_NO_SURFACE;
        GLuint textureId = 0;
        bool boundForRead = false;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyedMutex;
    };

    bool createProgram(QString *error);
    bool initialize(IGlPresentationTarget *target, QString *error, QString *runtimeLog);
    bool ensureDisplayTarget(const QSize &size, QString *error);
    bool copyFrameToDisplayTexture(const PublishedFrame &frame, QString *error);
    bool ensureImportedSlot(int slotIndex, QString *error);
    void consume(const PublishedFrame &frame);
    bool paint(PresentationFeedback *feedback, QString *error);
    void destroyImportedSlot(int slotIndex);
    void destroyDisplayTarget();

    IGlPresentationTarget *m_target = nullptr;
    QOpenGLFunctions *m_gl = nullptr;
    QOpenGLShaderProgram m_program;
    int m_positionLocation = -1;
    int m_texCoordLocation = -1;
    int m_samplerLocation = -1;
    std::shared_ptr<ISharedFrameSlotPool> m_slotPool;
    QtAngleEglTools::ResolvedEglApi *m_eglApi = nullptr;
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLConfig m_eglConfig = nullptr;
    QVector<ImportedSlot> m_importedSlots;
    std::unique_ptr<QtAngleEglTools::ResolvedEglApi> m_ownedEglApi;
    GLuint m_displayTextureId = 0U;
    GLuint m_displayFramebufferId = 0U;
    QSize m_displayTextureSize;
    PublishedFrame m_pendingFrame;
    bool m_hasPendingFrame = false;
    PublishedFrame m_displayFrame;
    bool m_hasDisplayFrame = false;
    bool m_initialized = false;
    QString m_runtimeLog;
};
