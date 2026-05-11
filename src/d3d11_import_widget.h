#pragma once

#include "src/framework/core/display_presenter.h"
#include "src/framework/core/shared_frame_slot_pool.h"
#include "src/framework/qt/qopenglwidget_display_host.h"

#include <QOpenGLWidget>
#include <memory>

class D3D11ImportWidget final : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit D3D11ImportWidget(QWidget *parent = nullptr);
    ~D3D11ImportWidget() override;

    void setSlotPool(const std::shared_ptr<ISharedFrameSlotPool> &slotPool);
    QSize outputPixelSize() const;

signals:
    void glInitialized();
    void displayReadyForWorker();
    void slotAvailableForWorker();
    void outputSizeChanged(QSize size);

public slots:
    void onFrameReady(int slotIndex, quint64 generation, QSize size, quint64 frameIndex);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

private slots:
    void notifyDisplayReadyForWorker();

private:
    std::shared_ptr<ISharedFrameSlotPool> m_slotPool;
    QOpenGLWidgetDisplayHost m_displayHost;
    std::unique_ptr<IFramePresenter> m_presenter;
    bool m_workerReadyPending = false;
    bool m_shuttingDown = false;
};
