#pragma once

#include "framework/core/display_presenter.h"
#include "framework/core/shared_frame_slot_pool.h"
#include "framework/qt/qopenglwidget_display_host.h"

#include <QOpenGLWidget>
#include <memory>

class QOpenGLWidgetFrameView final : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit QOpenGLWidgetFrameView(QWidget *parent = nullptr);
    ~QOpenGLWidgetFrameView() override;

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
