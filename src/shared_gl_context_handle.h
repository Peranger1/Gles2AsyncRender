#pragma once

#include <QObject>

class QOffscreenSurface;
class QOpenGLContext;

class SharedGlContextHandle final : public QObject
{
    Q_OBJECT

public:
    explicit SharedGlContextHandle(QObject *parent = nullptr);
    ~SharedGlContextHandle() override;

    void adopt(QOpenGLContext *context, QOffscreenSurface *surface);
    QOpenGLContext *context() const;
    QOffscreenSurface *surface() const;
    bool makeCurrent();
    void doneCurrent();
    void shutdown();

private:
    QOpenGLContext *m_context = nullptr;
    QOffscreenSurface *m_surface = nullptr;
};

Q_DECLARE_METATYPE(SharedGlContextHandle *)
