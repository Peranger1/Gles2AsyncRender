#pragma once

#include <QSize>
#include <QString>

#include <QtGui/qopengl.h>

#include <memory>

class D3D11NativeSlotPool;
class QOpenGLContext;

class AngleSharedTexturePublishBridge
{
public:
    AngleSharedTexturePublishBridge();
    ~AngleSharedTexturePublishBridge();

    bool initialize(QOpenGLContext *context,
                    D3D11NativeSlotPool *slotPool,
                    QString *error);

    bool publishToSlot(GLuint sourceTextureId,
                       const QSize &sourceSize,
                       int slotIndex,
                       QString *error);

    void releaseGlResources();

private:
    struct Impl;

    std::unique_ptr<Impl> m_impl;
};
