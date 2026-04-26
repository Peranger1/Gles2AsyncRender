#include <QApplication>
#include <QMetaType>
#include <QSurfaceFormat>

#include "image_effect_types.h"
#include "main_window.h"
#include "shared_gl_context_handle.h"
#include "shared_texture_frame_pool.h"

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
    qRegisterMetaType<SharedGlContextHandle *>("SharedGlContextHandle*");
    qRegisterMetaType<SharedTextureFramePool *>("SharedTextureFramePool*");

    MainWindow window;
    window.show();

    return app.exec();
}
