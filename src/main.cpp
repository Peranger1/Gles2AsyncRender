#include <QApplication>
#include <QMetaType>
#include <QSurfaceFormat>

#include "app/async_render_main_window.h"
#include "framework/backend/platform_render_backend.h"
#include "framework/core/shared_frame_slot_pool.h"
#include "image_effect_types.h"

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(2, 0);
    format.setProfile(QSurfaceFormat::NoProfile);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(0);
    format.setRedBufferSize(8);
    format.setGreenBufferSize(8);
    format.setBlueBufferSize(8);
    format.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Gles2AsyncRender"));
    qRegisterMetaType<ImageEffectParameters>("ImageEffectParameters");
    qRegisterMetaType<IPlatformRenderBackend *>("IPlatformRenderBackend*");
    qRegisterMetaType<FrameTicket>("FrameTicket");
    qRegisterMetaType<JobResult>("JobResult");
    qRegisterMetaType<quint64>("quint64");

    AsyncRenderMainWindow window;
    window.show();

    return app.exec();
}
