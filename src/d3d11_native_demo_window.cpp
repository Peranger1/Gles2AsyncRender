#include "d3d11_native_demo_window.h"

#include "d3d11_import_widget.h"
#include "d3d11_native_slot_pool.h"
#include "d3d11_native_worker.h"

#include <QAction>
#include <QDockWidget>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <QtMath>

D3D11NativeDemoWindow::D3D11NativeDemoWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_displayWidget(new D3D11ImportWidget(this))
    , m_slotPool(std::make_shared<D3D11NativeSlotPool>(3))
    , m_worker(new D3D11NativeWorker())
{
    setWindowTitle(QStringLiteral("Gles2AsyncRender"));
    resize(1280, 760);

    m_displayWidget->setSlotPool(m_slotPool);
    setCentralWidget(m_displayWidget);

    m_workerThread.setObjectName(QStringLiteral("D3D11NativeWorkerThread"));
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_displayWidget, &D3D11ImportWidget::glInitialized,
            this, &D3D11NativeDemoWindow::onDisplayGlInitialized,
            Qt::QueuedConnection);
    connect(m_displayWidget, &D3D11ImportWidget::displayReadyForWorker,
            this, &D3D11NativeDemoWindow::onDisplayReadyForWorker,
            Qt::QueuedConnection);
    connect(m_displayWidget, &D3D11ImportWidget::outputSizeChanged,
            m_worker, &D3D11NativeWorker::setOutputSize,
            Qt::QueuedConnection);
    connect(m_displayWidget, &D3D11ImportWidget::statusMessage,
            this, &D3D11NativeDemoWindow::onWorkerStatus,
            Qt::QueuedConnection);
    connect(m_displayWidget, &D3D11ImportWidget::framePresented,
            this, &D3D11NativeDemoWindow::onFramePresented,
            Qt::QueuedConnection);

    connect(m_worker, &D3D11NativeWorker::frameReady,
            m_displayWidget, &D3D11ImportWidget::onFrameReady,
            Qt::QueuedConnection);
    connect(m_worker, &D3D11NativeWorker::initializationFailed,
            this, &D3D11NativeDemoWindow::onWorkerError,
            Qt::QueuedConnection);
    connect(m_worker, &D3D11NativeWorker::statusMessage,
            this, &D3D11NativeDemoWindow::onWorkerStatus,
            Qt::QueuedConnection);
    connect(m_worker, &D3D11NativeWorker::imageDirectoryLoadFinished,
            this, &D3D11NativeDemoWindow::onImageDirectoryLoadFinished,
            Qt::QueuedConnection);
    connect(m_worker, &D3D11NativeWorker::imageSelectionChanged,
            this, &D3D11NativeDemoWindow::onImageSelectionChanged,
            Qt::QueuedConnection);
    connect(m_worker, &D3D11NativeWorker::renderTimingUpdated,
            this, &D3D11NativeDemoWindow::onRenderTimingUpdated,
            Qt::QueuedConnection);

    m_workerThread.start();

    setupActions();
    setupImageEffectControls();
    updateImageActions();
    updateStatusBarMessage(QStringLiteral("Waiting for D3D11 import widget initialization..."));
}

D3D11NativeDemoWindow::~D3D11NativeDemoWindow()
{
    if (m_workerThread.isRunning() && m_worker != nullptr) {
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait();
    }

    m_worker = nullptr;
}

void D3D11NativeDemoWindow::onDisplayGlInitialized()
{
    updateStatusBarMessage(QStringLiteral("Display import widget is ready. Waiting for the first frame swap before starting the D3D11 worker..."));
}

void D3D11NativeDemoWindow::onDisplayReadyForWorker()
{
    if (m_workerInitialized || m_workerInitAttempted || m_worker == nullptr) {
        return;
    }

    m_workerInitAttempted = true;

    bool initialized = false;
    const bool invoked = QMetaObject::invokeMethod(
        m_worker,
        "initialize",
        Qt::BlockingQueuedConnection,
        Q_RETURN_ARG(bool, initialized),
        Q_ARG(D3D11NativeSlotPool *, m_slotPool.get()),
        Q_ARG(QSize, m_displayWidget->outputPixelSize()));
    initialized = invoked && initialized;
    if (!initialized) {
        updateStatusBarMessage(QStringLiteral("D3D11 native worker initialization failed."));
        return;
    }

    m_workerInitialized = true;
    pushEffectParameters();
    requestRender();
    updateImageActions();
    updateStatusBarMessage(QStringLiteral("D3D11 native worker is ready. Import an image directory to start the demo."));
}

void D3D11NativeDemoWindow::openImageDirectory()
{
    if (!m_workerInitialized) {
        QMessageBox::warning(this, QStringLiteral("Worker Not Ready"), QStringLiteral("The D3D11 native worker is not initialized yet."));
        return;
    }

    const QString directoryPath = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Image Directory"),
        {},
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (directoryPath.isEmpty()) {
        return;
    }

    QMetaObject::invokeMethod(
        m_worker,
        "loadImageDirectory",
        Qt::QueuedConnection,
        Q_ARG(QString, directoryPath));
    updateStatusBarMessage(QStringLiteral("Loading image directory into D3D11 worker..."));
}

void D3D11NativeDemoWindow::showNextImage()
{
    if (!m_workerInitialized || m_imageCount <= 0) {
        return;
    }

    QMetaObject::invokeMethod(m_worker, "selectNextImage", Qt::QueuedConnection);
}

void D3D11NativeDemoWindow::showPreviousImage()
{
    if (!m_workerInitialized || m_imageCount <= 0) {
        return;
    }

    QMetaObject::invokeMethod(m_worker, "selectPreviousImage", Qt::QueuedConnection);
}

void D3D11NativeDemoWindow::onImageEffectControlChanged()
{
    m_effectParameters.brightness = float(m_brightnessSlider->value()) / 100.0f;
    m_effectParameters.contrast = float(m_contrastSlider->value()) / 100.0f;
    m_effectParameters.zoom = float(m_zoomSlider->value()) / 100.0f;
    m_effectParameters.panX = float(m_panXSlider->value()) / 100.0f;
    m_effectParameters.panY = float(m_panYSlider->value()) / 100.0f;
    m_effectParameters.rotationDegrees = float(m_rotationSlider->value());
    m_effectParameters.heavyGpuPassCount = m_heavyGpuSlider->value();
    setImageEffectControlsFromState();
    pushEffectParameters();
    requestRender();
}

void D3D11NativeDemoWindow::resetImageEffects()
{
    m_effectParameters = {};
    setImageEffectControlsFromState();
    pushEffectParameters();
    requestRender();
}

void D3D11NativeDemoWindow::onImageDirectoryLoadFinished(bool loaded,
                                                         const QString &errorMessage,
                                                         int currentIndex,
                                                         int count,
                                                         const QString &displayName)
{
    if (!loaded) {
        QMessageBox::warning(this,
                             QStringLiteral("Image Loading Failed"),
                             errorMessage.isEmpty()
                                 ? QStringLiteral("No images could be loaded from the selected directory.")
                                 : errorMessage);
        m_currentImageIndex = -1;
        m_imageCount = 0;
        m_currentImageName.clear();
        updateImageActions();
        updateStatusBarMessage(QStringLiteral("Image directory loading failed."));
        return;
    }

    m_currentImageIndex = currentIndex;
    m_imageCount = count;
    m_currentImageName = displayName;
    updateImageActions();
    updateStatusBarMessage();
}

void D3D11NativeDemoWindow::onImageSelectionChanged(int currentIndex, int count, const QString &displayName)
{
    m_currentImageIndex = currentIndex;
    m_imageCount = count;
    m_currentImageName = displayName;
    updateImageActions();
    updateStatusBarMessage();
}

void D3D11NativeDemoWindow::onWorkerError(const QString &reason)
{
    updateStatusBarMessage(QStringLiteral("Worker error: %1").arg(reason));
    QMessageBox::warning(this, QStringLiteral("Worker Error"), reason);
}

void D3D11NativeDemoWindow::onWorkerStatus(const QString &message)
{
    if (!message.isEmpty()) {
        updateStatusBarMessage(message);
    }
}

void D3D11NativeDemoWindow::onRenderTimingUpdated(double elapsedMs)
{
    m_lastRenderElapsedMs = elapsedMs;
    updateStatusBarMessage();
}

void D3D11NativeDemoWindow::onFramePresented(quint64 frameIndex, QSize size)
{
    Q_UNUSED(frameIndex);
    m_lastPresentedSize = size;
    updateStatusBarMessage();
}

void D3D11NativeDemoWindow::setupActions()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
    m_openDirectoryAction = fileMenu->addAction(QStringLiteral("Open Image Directory"));
    m_openDirectoryAction->setShortcut(QKeySequence::Open);
    connect(m_openDirectoryAction, &QAction::triggered, this, &D3D11NativeDemoWindow::openImageDirectory);

    QMenu *imageMenu = menuBar()->addMenu(QStringLiteral("Image"));
    m_previousImageAction = imageMenu->addAction(QStringLiteral("Previous"));
    m_previousImageAction->setShortcut(QKeySequence::MoveToPreviousPage);
    connect(m_previousImageAction, &QAction::triggered, this, &D3D11NativeDemoWindow::showPreviousImage);

    m_nextImageAction = imageMenu->addAction(QStringLiteral("Next"));
    m_nextImageAction->setShortcut(QKeySequence::MoveToNextPage);
    connect(m_nextImageAction, &QAction::triggered, this, &D3D11NativeDemoWindow::showNextImage);

    imageMenu->addSeparator();
    QAction *resetEffectsAction = imageMenu->addAction(QStringLiteral("Reset Effects"));
    connect(resetEffectsAction, &QAction::triggered, this, &D3D11NativeDemoWindow::resetImageEffects);

    QMenu *renderMenu = menuBar()->addMenu(QStringLiteral("Render"));
    QAction *requestFrameAction = renderMenu->addAction(QStringLiteral("Render Once"));
    connect(requestFrameAction, &QAction::triggered, this, &D3D11NativeDemoWindow::requestRender);
}

void D3D11NativeDemoWindow::setupImageEffectControls()
{
    m_imageEffectDock = new QDockWidget(QStringLiteral("Producer Controls"), this);
    m_imageEffectDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_imageEffectDock->setMaximumWidth(400);

    QWidget *panel = new QWidget(m_imageEffectDock);
    QVBoxLayout *layout = new QVBoxLayout(panel);

    auto addSlider = [panel, layout](const QString &labelText, int min, int max, int value, QLabel **valueLabel) {
        QLabel *label = new QLabel(labelText, panel);
        QSlider *slider = new QSlider(Qt::Horizontal, panel);
        slider->setRange(min, max);
        slider->setValue(value);
        QLabel *display = new QLabel(panel);
        QHBoxLayout *rowLayout = new QHBoxLayout();

        layout->addWidget(label);
        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(display);
        layout->addLayout(rowLayout);
        *valueLabel = display;
        return slider;
    };

    m_brightnessSlider = addSlider(QStringLiteral("Brightness"), -100, 100, 0, &m_brightnessValueLabel);
    m_contrastSlider = addSlider(QStringLiteral("Contrast"), 0, 200, 100, &m_contrastValueLabel);
    m_zoomSlider = addSlider(QStringLiteral("Zoom"), 10, 900, 100, &m_zoomValueLabel);
    m_panXSlider = addSlider(QStringLiteral("Pan X"), -100, 100, 0, &m_panXValueLabel);
    m_panYSlider = addSlider(QStringLiteral("Pan Y"), -100, 100, 0, &m_panYValueLabel);
    m_rotationSlider = addSlider(QStringLiteral("Rotation"), -180, 180, 0, &m_rotationValueLabel);
    m_heavyGpuSlider = addSlider(QStringLiteral("GPU Stress Loops"), 0, 512, 0, &m_heavyGpuValueLabel);

    QLabel *flipLabel = new QLabel(QStringLiteral("Flip"), panel);
    QHBoxLayout *flipLayout = new QHBoxLayout();
    m_flipHorizontalButton = new QPushButton(QStringLiteral("Flip H"), panel);
    m_flipVerticalButton = new QPushButton(QStringLiteral("Flip V"), panel);
    m_flipHorizontalButton->setCheckable(true);
    m_flipVerticalButton->setCheckable(true);
    flipLayout->addWidget(m_flipHorizontalButton);
    flipLayout->addWidget(m_flipVerticalButton);

    layout->addWidget(flipLabel);
    layout->addLayout(flipLayout);
    layout->addWidget(new QLabel(QStringLiteral("GPU Stress Loops runs additional pixel-shader iterations on the D3D11 worker before ReleaseSync(1)."), panel));
    layout->addWidget(new QLabel(QStringLiteral("This path keeps ANGLE/QOpenGLWidget only on the consumer side and removes worker-side GLES context contention."), panel));
    layout->addStretch(1);

    connect(m_brightnessSlider, &QSlider::valueChanged, this, &D3D11NativeDemoWindow::onImageEffectControlChanged);
    connect(m_contrastSlider, &QSlider::valueChanged, this, &D3D11NativeDemoWindow::onImageEffectControlChanged);
    connect(m_zoomSlider, &QSlider::valueChanged, this, &D3D11NativeDemoWindow::onImageEffectControlChanged);
    connect(m_panXSlider, &QSlider::valueChanged, this, &D3D11NativeDemoWindow::onImageEffectControlChanged);
    connect(m_panYSlider, &QSlider::valueChanged, this, &D3D11NativeDemoWindow::onImageEffectControlChanged);
    connect(m_rotationSlider, &QSlider::valueChanged, this, &D3D11NativeDemoWindow::onImageEffectControlChanged);
    connect(m_heavyGpuSlider, &QSlider::valueChanged, this, &D3D11NativeDemoWindow::onImageEffectControlChanged);
    connect(m_flipHorizontalButton, &QPushButton::toggled, this, [this](bool checked) {
        m_effectParameters.flipHorizontal = checked;
        setImageEffectControlsFromState();
        pushEffectParameters();
        requestRender();
    });
    connect(m_flipVerticalButton, &QPushButton::toggled, this, [this](bool checked) {
        m_effectParameters.flipVertical = checked;
        setImageEffectControlsFromState();
        pushEffectParameters();
        requestRender();
    });

    m_imageEffectDock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, m_imageEffectDock);
    setImageEffectControlsFromState();
}

void D3D11NativeDemoWindow::setImageEffectControlsFromState()
{
    const QSignalBlocker brightnessBlocker(m_brightnessSlider);
    const QSignalBlocker contrastBlocker(m_contrastSlider);
    const QSignalBlocker zoomBlocker(m_zoomSlider);
    const QSignalBlocker panXBlocker(m_panXSlider);
    const QSignalBlocker panYBlocker(m_panYSlider);
    const QSignalBlocker rotationBlocker(m_rotationSlider);
    const QSignalBlocker heavyGpuBlocker(m_heavyGpuSlider);
    const QSignalBlocker flipHorizontalBlocker(m_flipHorizontalButton);
    const QSignalBlocker flipVerticalBlocker(m_flipVerticalButton);

    m_brightnessSlider->setValue(int(qRound(m_effectParameters.brightness * 100.0f)));
    m_contrastSlider->setValue(int(qRound(m_effectParameters.contrast * 100.0f)));
    m_zoomSlider->setValue(int(qRound(m_effectParameters.zoom * 100.0f)));
    m_panXSlider->setValue(int(qRound(m_effectParameters.panX * 100.0f)));
    m_panYSlider->setValue(int(qRound(m_effectParameters.panY * 100.0f)));
    m_rotationSlider->setValue(int(qRound(m_effectParameters.rotationDegrees)));
    m_heavyGpuSlider->setValue(m_effectParameters.heavyGpuPassCount);
    m_flipHorizontalButton->setChecked(m_effectParameters.flipHorizontal);
    m_flipVerticalButton->setChecked(m_effectParameters.flipVertical);
    m_flipHorizontalButton->setText(m_effectParameters.flipHorizontal ? QStringLiteral("Flip H On") : QStringLiteral("Flip H"));
    m_flipVerticalButton->setText(m_effectParameters.flipVertical ? QStringLiteral("Flip V On") : QStringLiteral("Flip V"));

    m_brightnessValueLabel->setText(QString::number(m_effectParameters.brightness, 'f', 2));
    m_contrastValueLabel->setText(QString::number(m_effectParameters.contrast, 'f', 2));
    m_zoomValueLabel->setText(QString::number(m_effectParameters.zoom, 'f', 2));
    m_panXValueLabel->setText(QString::number(m_effectParameters.panX, 'f', 2));
    m_panYValueLabel->setText(QString::number(m_effectParameters.panY, 'f', 2));
    m_rotationValueLabel->setText(QStringLiteral("%1 deg").arg(int(qRound(m_effectParameters.rotationDegrees))));
    m_heavyGpuValueLabel->setText(QStringLiteral("%1 loops").arg(m_effectParameters.heavyGpuPassCount));
}

void D3D11NativeDemoWindow::updateStatusBarMessage(const QString &message)
{
    if (!message.isEmpty()) {
        statusBar()->showMessage(message);
        return;
    }

    if (!m_workerInitialized) {
        statusBar()->showMessage(QStringLiteral("Waiting for D3D11 native worker initialization..."));
        return;
    }

    if (m_imageCount <= 0) {
        statusBar()->showMessage(QStringLiteral("D3D11 native worker is ready. Import an image directory to start the demo."));
        return;
    }

    statusBar()->showMessage(QStringLiteral("%1 / %2  %3  Output:%4x%5  Worker:%6 ms  Stress:%7 loops")
        .arg(m_currentImageIndex + 1)
        .arg(m_imageCount)
        .arg(m_currentImageName)
        .arg(m_lastPresentedSize.isValid() ? m_lastPresentedSize.width() : m_displayWidget->outputPixelSize().width())
        .arg(m_lastPresentedSize.isValid() ? m_lastPresentedSize.height() : m_displayWidget->outputPixelSize().height())
        .arg(QString::number(m_lastRenderElapsedMs, 'f', 1))
        .arg(m_effectParameters.heavyGpuPassCount));
}

void D3D11NativeDemoWindow::updateImageActions()
{
    const bool ready = m_workerInitialized && m_imageCount > 0;
    if (m_previousImageAction != nullptr) {
        m_previousImageAction->setEnabled(ready);
    }
    if (m_nextImageAction != nullptr) {
        m_nextImageAction->setEnabled(ready);
    }
    if (m_openDirectoryAction != nullptr) {
        m_openDirectoryAction->setEnabled(m_workerInitialized);
    }
}

void D3D11NativeDemoWindow::pushEffectParameters()
{
    if (!m_workerInitialized || m_worker == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(
        m_worker,
        "setEffectParameters",
        Qt::QueuedConnection,
        Q_ARG(ImageEffectParameters, m_effectParameters));
}

void D3D11NativeDemoWindow::requestRender()
{
    if (!m_workerInitialized || m_worker == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(m_worker, "requestRender", Qt::QueuedConnection);
}
