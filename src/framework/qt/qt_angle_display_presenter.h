#pragma once

#include "framework/core/frame_presenter.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <functional>
#include <memory>

#include <QtANGLE/EGL/egl.h>

#include <wrl/client.h>

struct IDXGIKeyedMutex;

namespace QtAngleEglTools
{
struct ResolvedEglApi;
}

class QtAngleDisplayPresenter final : public IFramePresenter
{
public:
    QtAngleDisplayPresenter();
    ~QtAngleDisplayPresenter();

    QString lastRuntimeLog() const;
    bool initialize(IDisplayTarget &target,
                    IFrameReader &frameReader,
                    QString *error) override;
    bool enqueue(const FrameTicket &ticket, QString *error) override;
    bool present(FramePresentationFeedback *feedback, QString *error) override;
    QString diagnosticText() const override;
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

    struct GlTargetContext final
    {
        QOpenGLContext *context = nullptr;
        QOpenGLFunctions *functions = nullptr;
        std::function<QSize()> targetSize;
        std::function<void()> requestPresent;
    };

    bool createProgram(QString *error);
    bool initialize(const GlTargetContext &targetContext,
                    IFrameReader *frameReader,
                    QString *error,
                    QString *runtimeLog);
    bool ensureDisplayTarget(const QSize &size, QString *error);
    bool copyFrameToDisplayTexture(const FrameTicket &frame, QString *error);
    bool ensureImportedSlot(int slotIndex, QString *error);
    void consume(const FrameTicket &frame);
    bool paint(FramePresentationFeedback *feedback, QString *error);
    void destroyImportedSlot(int slotIndex);
    void destroyDisplayTarget();

    QSize currentTargetSize() const;
    void requestPresentUpdate();

    QOpenGLContext *m_targetContext = nullptr;
    QOpenGLFunctions *m_gl = nullptr;
    QOpenGLShaderProgram m_program;
    int m_positionLocation = -1;
    int m_texCoordLocation = -1;
    int m_samplerLocation = -1;
    IFrameReader *m_frameReader = nullptr;
    QtAngleEglTools::ResolvedEglApi *m_eglApi = nullptr;
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLConfig m_eglConfig = nullptr;
    QVector<ImportedSlot> m_importedSlots;
    std::unique_ptr<QtAngleEglTools::ResolvedEglApi> m_ownedEglApi;
    GLuint m_displayTextureId = 0U;
    GLuint m_displayFramebufferId = 0U;
    QSize m_displayTextureSize;
    std::function<QSize()> m_targetSizeProvider;
    std::function<void()> m_requestPresent;
    FrameTicket m_pendingFrame;
    bool m_hasPendingFrame = false;
    FrameTicket m_displayFrame;
    bool m_hasDisplayFrame = false;
    bool m_initialized = false;
    QString m_runtimeLog;
};
