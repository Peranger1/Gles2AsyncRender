#include <QApplication>
#include <QMainWindow>
#include <QSurfaceFormat>

#include "async_gles_widget.h"

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

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("QOpenGLWidget GLES2 Async Shared Texture Demo"));
    window.resize(1280, 720);
    window.setCentralWidget(new AsyncGlesWidget(&window));
    window.show();

    return app.exec();
}
