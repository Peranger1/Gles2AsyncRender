#pragma once

#include "image_effect_types.h"

#include <QMainWindow>
#include <QThread>

class QAction;
class AsyncGlesWidget;
class QLabel;
class QPushButton;
class QSlider;
class QDockWidget;
class SharedGlEnvironment;
class SharedTextureWorker;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onDisplayGlInitialized();
    void onDisplayReadyForWorker();
    void openImageDirectory();
    void showNextImage();
    void showPreviousImage();
    void onImageEffectControlChanged();
    void resetImageEffects();
    void onImageDirectoryLoadFinished(
        bool loaded,
        const QString &errorMessage,
        int currentIndex,
        int count,
        const QString &displayName);
    void onImageSelectionChanged(int currentIndex, int count, const QString &displayName);
    void onWorkerError(const QString &reason);
    void onWorkerStatus(const QString &message);
    void onRenderTimingUpdated(double elapsedMs);

private:
    void setupActions();
    void setupImageEffectControls();
    void setImageEffectControlsFromState();
    void updateImageActions();
    void updateStatusBarMessage(const QString &message = QString());
    void pushEffectParameters();
    void requestRender();

    AsyncGlesWidget *m_displayWidget = nullptr;
    SharedGlEnvironment *m_sharedGlEnvironment = nullptr;
    SharedTextureWorker *m_worker = nullptr;
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
};
