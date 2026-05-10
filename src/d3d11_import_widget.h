#pragma once

#include "d3d11_native_slot_pool.h"

#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QOpenGLShaderProgram>
#include <memory>

#include <QtANGLE/EGL/egl.h>

#include <wrl/client.h>

struct IDXGIKeyedMutex;

namespace QtAngleEglTools
{
struct ResolvedEglApi;
}

class D3D11ImportWidget final : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit D3D11ImportWidget(QWidget *parent = nullptr);
    ~D3D11ImportWidget() override;

    void setSlotPool(const std::shared_ptr<D3D11NativeSlotPool> &slotPool);
    QSize outputPixelSize() const;

signals:
    void glInitialized();
    void displayReadyForWorker();
    void outputSizeChanged(QSize size);

public slots:
    void onFrameReady(int slotIndex, quint64 generation, QSize size, quint64 frameIndex);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

private slots:
    void notifyDisplayReadyForWorker();
    void onFrameSwapped();

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
    bool ensureImportedSlot(int slotIndex, QString *error);
    void destroyImportedSlot(int slotIndex);
    void releaseImportedSlotReadback(int slotIndex);

    QOpenGLShaderProgram m_program;
    int m_positionLocation = -1;
    int m_texCoordLocation = -1;
    int m_samplerLocation = -1;
    std::shared_ptr<D3D11NativeSlotPool> m_slotPool;
    QtAngleEglTools::ResolvedEglApi *m_eglApi = nullptr;
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLConfig m_eglConfig = nullptr;
    QVector<ImportedSlot> m_importedSlots;
    std::unique_ptr<QtAngleEglTools::ResolvedEglApi> m_ownedEglApi;
    D3D11NativeFrame m_pendingFrame;
    bool m_hasPendingFrame = false;
    D3D11NativeFrame m_frontFrame;
    bool m_hasFrontFrame = false;
    D3D11NativeFrame m_retiringFrame;
    bool m_hasRetiringFrame = false;
    bool m_workerReadyPending = false;
};
