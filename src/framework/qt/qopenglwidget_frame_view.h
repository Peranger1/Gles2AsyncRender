#pragma once

#include "framework/core/frame_presenter.h"

#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <memory>

class QOpenGLWidgetFrameView final : public QOpenGLWidget, public IGlDisplayTarget
{
    Q_OBJECT

public:
    explicit QOpenGLWidgetFrameView(QWidget *parent = nullptr);
    ~QOpenGLWidgetFrameView() override;

    void setFrameReader(const std::shared_ptr<IFrameReader> &frameReader);
    void setPresenter(std::unique_ptr<IFramePresenter> presenter);
    QSize outputPixelSize() const;
    QSize targetSize() const override;
    void requestPresent() override;
    QOpenGLContext *glContext() const override;
    QOpenGLFunctions *glFunctions() const override;

signals:
    void glInitialized();
    void displayReadyForWorker();
    void publicationCapacityAvailable();
    void outputSizeChanged(QSize size);

public slots:
    void onFrameReady(const FrameTicket &ticket);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

private slots:
    void notifyDisplayReadyForWorker();

private:
    std::shared_ptr<IFrameReader> m_frameReader;
    std::unique_ptr<IFramePresenter> m_presenter;
    bool m_workerReadyPending = false;
    bool m_shuttingDown = false;
};
