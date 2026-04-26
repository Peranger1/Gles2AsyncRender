#pragma once

#include <QObject>
#include <QSurfaceFormat>
#include <QVector>

class QOffscreenSurface;
class QOpenGLContext;
class SharedGlContextHandle;

class SharedGlEnvironment final : public QObject
{
    Q_OBJECT

public:
    explicit SharedGlEnvironment(QObject *parent = nullptr);
    ~SharedGlEnvironment() override;

    bool initializeFromDisplay(QOpenGLContext *displayContext, const QSurfaceFormat &format);
    SharedGlContextHandle *createSharedContext(QObject *parent = nullptr);
    bool isInitialized() const;

private:
    QOpenGLContext *m_displayContext = nullptr;
    QSurfaceFormat m_format;
    QVector<QOffscreenSurface *> m_surfaces;
};
