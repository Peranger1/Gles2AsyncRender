#pragma once

#include "image_effect_types.h"

#include <QImage>
#include <QMainWindow>
#include <QSize>
#include <QThread>
#include <memory>

class QAction;
class IPlatformBackend;
class TexturePresentWidget;
class PhotoEditorAppSession;
class QLabel;
class QDockWidget;
class QPushButton;
class QSlider;
class QString;

class AsyncRenderMainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit AsyncRenderMainWindow(QWidget *parent = nullptr);
    ~AsyncRenderMainWindow() override;

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
                                      const QString &displayName,
                                      QSize imageSize);
    void onImageSelectionChanged(int currentIndex, int count, const QString &displayName, QSize imageSize);
    void onWorkerError(const QString &reason);
    void onWorkerWarning(const QString &reason);
    void showCpuPreviewDialog(const QImage &image, const QString &description);
    void runCpuPreviewInspection();

private:
    void setupActions();
    void setupImageEffectControls();
    void setImageEffectControlsFromState();
    void updateImageActions();
    void updateStatusBarMessage();
    void pushEffectParameters();
    void requestRender();

    TexturePresentWidget *m_displayWidget = nullptr;
    std::unique_ptr<IPlatformBackend> m_renderBackend;
    PhotoEditorAppSession *m_worker = nullptr;
    QThread m_workerThread;

    QAction *m_openDirectoryAction = nullptr;
    QAction *m_previousImageAction = nullptr;
    QAction *m_nextImageAction = nullptr;
    QAction *m_runCpuPreviewAction = nullptr;
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
    QSize m_currentImageSize;
};
