#pragma once

#include "image_effect_types.h"

#include <QMainWindow>
#include <QThread>
#include <memory>

class QAction;
class D3D11ImportWidget;
class D3D11NativeSlotPool;
class D3D11NativeWorker;
class QLabel;
class QDockWidget;
class QPushButton;
class QSlider;
class QString;

class D3D11NativeDemoWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit D3D11NativeDemoWindow(QWidget *parent = nullptr);
    ~D3D11NativeDemoWindow() override;

private slots:
    void onDisplayGlInitialized();
    void onDisplayReadyForWorker();
    void openImageDirectory();
    void showNextImage();
    void showPreviousImage();
    void onImageEffectControlChanged();
    void resetImageEffects();
    void onImageDirectoryLoadFinished(bool loaded,
                                      const QString &errorMessage,
                                      int currentIndex,
                                      int count,
                                      const QString &displayName);
    void onImageSelectionChanged(int currentIndex, int count, const QString &displayName);
    void onWorkerError(const QString &reason);
    void onWorkerStatus(const QString &message);
    void onRenderTimingUpdated(double elapsedMs);
    void onFramePresented(quint64 frameIndex, QSize size);

private:
    void setupActions();
    void setupImageEffectControls();
    void setImageEffectControlsFromState();
    void updateImageActions();
    void updateStatusBarMessage(const QString &message = QString());
    void pushEffectParameters();
    void requestRender();

    D3D11ImportWidget *m_displayWidget = nullptr;
    std::shared_ptr<D3D11NativeSlotPool> m_slotPool;
    D3D11NativeWorker *m_worker = nullptr;
    QThread m_workerThread;

    QAction *m_openDirectoryAction = nullptr;
    QAction *m_previousImageAction = nullptr;
    QAction *m_nextImageAction = nullptr;
    QDockWidget *m_imageEffectDock = nullptr;
    QLabel *m_brightnessValueLabel = nullptr;
    QLabel *m_contrastValueLabel = nullptr;
    QLabel *m_zoomValueLabel = nullptr;
    QLabel *m_panXValueLabel = nullptr;
    QLabel *m_panYValueLabel = nullptr;
    QLabel *m_rotationValueLabel = nullptr;
    QLabel *m_heavyGpuValueLabel = nullptr;
    QSlider *m_brightnessSlider = nullptr;
    QSlider *m_contrastSlider = nullptr;
    QSlider *m_zoomSlider = nullptr;
    QSlider *m_panXSlider = nullptr;
    QSlider *m_panYSlider = nullptr;
    QSlider *m_rotationSlider = nullptr;
    QSlider *m_heavyGpuSlider = nullptr;
    QPushButton *m_flipHorizontalButton = nullptr;
    QPushButton *m_flipVerticalButton = nullptr;

    ImageEffectParameters m_effectParameters;
    bool m_workerInitialized = false;
    bool m_workerInitAttempted = false;
    int m_currentImageIndex = -1;
    int m_imageCount = 0;
    QString m_currentImageName;
    double m_lastRenderElapsedMs = 0.0;
    QSize m_lastPresentedSize;
};
