#include <QApplication>
#include <QMetaType>
#include <QSurfaceFormat>

#include "app/async_render_main_window.h"
#include "framework/platform/platform_backend.h"
#include "framework/platform/texture_types.h"
#include "image_effect_types.h"

int main(int argc, char *argv[])
{
#if defined(Q_OS_WIN)
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);
#endif
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QSurfaceFormat format;
#if defined(Q_OS_WIN)
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(2, 0);
    format.setProfile(QSurfaceFormat::NoProfile);
#else
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::NoProfile);
#endif
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
    qRegisterMetaType<IPlatformBackend *>("IPlatformBackend*");
    qRegisterMetaType<TextureTicket>("TextureTicket");
    qRegisterMetaType<quint64>("quint64");

    AsyncRenderMainWindow window;
    window.show();

    return app.exec();
}
