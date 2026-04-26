#include "main_window.h"

#include "async_gles_widget.h"
#include "shared_gl_context_handle.h"
#include "shared_gl_environment.h"
#include "shared_texture_frame_pool.h"
#include "shared_texture_worker.h"

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
#include <QVBoxLayout>
#include <QWidget>
#include <QtMath>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_displayWidget(new AsyncGlesWidget(this))
    , m_sharedGlEnvironment(new SharedGlEnvironment(this))
    , m_worker(new SharedTextureWorker())
{
    setWindowTitle(QStringLiteral("GLES2 Async Image Processing Demo"));
    resize(1280, 760);

    m_displayWidget->setSharedGlEnvironment(m_sharedGlEnvironment);
    setCentralWidget(m_displayWidget);

    m_workerThread.setObjectName(QStringLiteral("SharedTextureWorkerThread"));
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_displayWidget, &AsyncGlesWidget::glInitialized,
            this, &MainWindow::onDisplayGlInitialized,
            Qt::QueuedConnection);
    connect(m_displayWidget, &AsyncGlesWidget::displayReadyForWorker,
            this, &MainWindow::onDisplayReadyForWorker,
            Qt::QueuedConnection);
    connect(m_displayWidget, &AsyncGlesWidget::outputSizeChanged,
            m_worker, &SharedTextureWorker::setOutputSize,
            Qt::QueuedConnection);

    connect(m_worker, &SharedTextureWorker::textureReady,
            m_displayWidget, &AsyncGlesWidget::onTextureReady,
            Qt::QueuedConnection);
    connect(m_worker, &SharedTextureWorker::initializationFailed,
            this, &MainWindow::onWorkerError,
            Qt::QueuedConnection);
    connect(m_worker, &SharedTextureWorker::statusMessage,
            this, &MainWindow::onWorkerStatus,
            Qt::QueuedConnection);
    connect(m_worker, &SharedTextureWorker::imageDirectoryLoadFinished,
            this, &MainWindow::onImageDirectoryLoadFinished,
            Qt::QueuedConnection);
    connect(m_worker, &SharedTextureWorker::imageSelectionChanged,
            this, &MainWindow::onImageSelectionChanged,
            Qt::QueuedConnection);

    m_workerThread.start();

    setupActions();
    setupImageEffectControls();
    updateImageActions();
    updateStatusBarMessage(QStringLiteral("Waiting for display OpenGL initialization..."));
}

MainWindow::~MainWindow()
{
    if (m_workerThread.isRunning() && m_worker != nullptr) {
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait();
    }

    m_worker = nullptr;
}

void MainWindow::onDisplayGlInitialized()
{
    updateStatusBarMessage(QStringLiteral("Display context is ready. Waiting for the first frame swap before starting the worker..."));
}

void MainWindow::onDisplayReadyForWorker()
{
    if (m_workerInitialized || m_workerInitAttempted || m_worker == nullptr) {
        return;
    }

    m_workerInitAttempted = true;

    SharedGlContextHandle *handle = m_sharedGlEnvironment->createSharedContext();
    bool initialized = false;
    if (handle != nullptr) {
        handle->moveToThread(&m_workerThread);
        if (handle->context() != nullptr) {
            handle->context()->moveToThread(&m_workerThread);
        }

        const bool invoked = QMetaObject::invokeMethod(
            m_worker,
            "initialize",
            Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(bool, initialized),
            Q_ARG(SharedGlContextHandle *, handle),
            Q_ARG(SharedTextureFramePool *, m_displayWidget->framePool()),
            Q_ARG(QSize, m_displayWidget->outputPixelSize()));
        initialized = invoked && initialized;
    }

    if (!initialized) {
        updateStatusBarMessage(QStringLiteral("Worker initialization failed."));
        return;
    }

    m_workerInitialized = true;
    pushEffectParameters();
    requestRender();
    updateImageActions();
    updateStatusBarMessage(QStringLiteral("Worker is ready. You can import an image directory now."));
}

void MainWindow::openImageDirectory()
{
    if (!m_workerInitialized) {
        QMessageBox::warning(this, QStringLiteral("Worker Not Ready"), QStringLiteral("The worker is not initialized yet."));
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
    updateStatusBarMessage(QStringLiteral("Loading image directory..."));
}

void MainWindow::showNextImage()
{
    if (!m_workerInitialized || m_imageCount <= 0) {
        return;
    }

    QMetaObject::invokeMethod(m_worker, "selectNextImage", Qt::QueuedConnection);
}

void MainWindow::showPreviousImage()
{
    if (!m_workerInitialized || m_imageCount <= 0) {
        return;
    }

    QMetaObject::invokeMethod(m_worker, "selectPreviousImage", Qt::QueuedConnection);
}

void MainWindow::onImageEffectControlChanged()
{
    m_effectParameters.brightness = float(m_brightnessSlider->value()) / 100.0f;
    m_effectParameters.contrast = float(m_contrastSlider->value()) / 100.0f;
    m_effectParameters.zoom = float(m_zoomSlider->value()) / 100.0f;
    m_effectParameters.panX = float(m_panXSlider->value()) / 100.0f;
    m_effectParameters.panY = float(m_panYSlider->value()) / 100.0f;
    m_effectParameters.rotationDegrees = float(m_rotationSlider->value());
    setImageEffectControlsFromState();
    pushEffectParameters();
    requestRender();
}

void MainWindow::resetImageEffects()
{
    m_effectParameters = {};
    setImageEffectControlsFromState();
    pushEffectParameters();
    requestRender();
}

void MainWindow::onImageDirectoryLoadFinished(
    bool loaded,
    const QString &errorMessage,
    int currentIndex,
    int count,
    const QString &displayName)
{
    if (!loaded) {
        QMessageBox::warning(
            this,
            QStringLiteral("Image Loading Failed"),
            errorMessage.isEmpty() ? QStringLiteral("No images could be loaded from the selected directory.") : errorMessage);
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

void MainWindow::onImageSelectionChanged(int currentIndex, int count, const QString &displayName)
{
    m_currentImageIndex = currentIndex;
    m_imageCount = count;
    m_currentImageName = displayName;
    updateImageActions();
    updateStatusBarMessage();
}

void MainWindow::onWorkerError(const QString &reason)
{
    updateStatusBarMessage(QStringLiteral("Worker error: %1").arg(reason));
    QMessageBox::warning(this, QStringLiteral("Worker Error"), reason);
}

void MainWindow::onWorkerStatus(const QString &message)
{
    if (!message.isEmpty()) {
        updateStatusBarMessage(message);
    }
}

void MainWindow::setupActions()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
    m_openDirectoryAction = fileMenu->addAction(QStringLiteral("Open Image Directory"));
    connect(m_openDirectoryAction, &QAction::triggered, this, &MainWindow::openImageDirectory);

    QMenu *imageMenu = menuBar()->addMenu(QStringLiteral("Image"));
    m_previousImageAction = imageMenu->addAction(QStringLiteral("Previous"));
    m_previousImageAction->setShortcut(QKeySequence::MoveToPreviousPage);
    connect(m_previousImageAction, &QAction::triggered, this, &MainWindow::showPreviousImage);

    m_nextImageAction = imageMenu->addAction(QStringLiteral("Next"));
    m_nextImageAction->setShortcut(QKeySequence::MoveToNextPage);
    connect(m_nextImageAction, &QAction::triggered, this, &MainWindow::showNextImage);

    imageMenu->addSeparator();
    QAction *resetEffectsAction = imageMenu->addAction(QStringLiteral("Reset Effects"));
    connect(resetEffectsAction, &QAction::triggered, this, &MainWindow::resetImageEffects);
}

void MainWindow::setupImageEffectControls()
{
    m_imageEffectDock = new QDockWidget(QStringLiteral("Image Effects"), this);
    m_imageEffectDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

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
    m_zoomSlider = addSlider(QStringLiteral("Zoom"), 10, 300, 100, &m_zoomValueLabel);
    m_panXSlider = addSlider(QStringLiteral("Pan X"), -100, 100, 0, &m_panXValueLabel);
    m_panYSlider = addSlider(QStringLiteral("Pan Y"), -100, 100, 0, &m_panYValueLabel);
    m_rotationSlider = addSlider(QStringLiteral("Rotation"), -180, 180, 0, &m_rotationValueLabel);

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
    layout->addWidget(new QLabel(QStringLiteral("All image effects run in the worker GL context."), panel));
    layout->addStretch(1);

    connect(m_brightnessSlider, &QSlider::valueChanged, this, &MainWindow::onImageEffectControlChanged);
    connect(m_contrastSlider, &QSlider::valueChanged, this, &MainWindow::onImageEffectControlChanged);
    connect(m_zoomSlider, &QSlider::valueChanged, this, &MainWindow::onImageEffectControlChanged);
    connect(m_panXSlider, &QSlider::valueChanged, this, &MainWindow::onImageEffectControlChanged);
    connect(m_panYSlider, &QSlider::valueChanged, this, &MainWindow::onImageEffectControlChanged);
    connect(m_rotationSlider, &QSlider::valueChanged, this, &MainWindow::onImageEffectControlChanged);
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

void MainWindow::setImageEffectControlsFromState()
{
    const QSignalBlocker brightnessBlocker(m_brightnessSlider);
    const QSignalBlocker contrastBlocker(m_contrastSlider);
    const QSignalBlocker zoomBlocker(m_zoomSlider);
    const QSignalBlocker panXBlocker(m_panXSlider);
    const QSignalBlocker panYBlocker(m_panYSlider);
    const QSignalBlocker rotationBlocker(m_rotationSlider);
    const QSignalBlocker flipHorizontalBlocker(m_flipHorizontalButton);
    const QSignalBlocker flipVerticalBlocker(m_flipVerticalButton);

    m_brightnessSlider->setValue(int(qRound(m_effectParameters.brightness * 100.0f)));
    m_contrastSlider->setValue(int(qRound(m_effectParameters.contrast * 100.0f)));
    m_zoomSlider->setValue(int(qRound(m_effectParameters.zoom * 100.0f)));
    m_panXSlider->setValue(int(qRound(m_effectParameters.panX * 100.0f)));
    m_panYSlider->setValue(int(qRound(m_effectParameters.panY * 100.0f)));
    m_rotationSlider->setValue(int(qRound(m_effectParameters.rotationDegrees)));
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
}

void MainWindow::updateImageActions()
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

void MainWindow::updateStatusBarMessage(const QString &message)
{
    if (!message.isEmpty()) {
        statusBar()->showMessage(message);
        return;
    }

    if (!m_workerInitialized) {
        statusBar()->showMessage(QStringLiteral("Waiting for worker initialization..."));
        return;
    }

    if (m_imageCount <= 0) {
        statusBar()->showMessage(QStringLiteral("Worker is ready. Import an image directory to start the demo."));
        return;
    }

    statusBar()->showMessage(QStringLiteral("%1 / %2  %3  Output:%4x%5")
        .arg(m_currentImageIndex + 1)
        .arg(m_imageCount)
        .arg(m_currentImageName)
        .arg(m_displayWidget->outputPixelSize().width())
        .arg(m_displayWidget->outputPixelSize().height()));
}

void MainWindow::pushEffectParameters()
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

void MainWindow::requestRender()
{
    if (!m_workerInitialized || m_worker == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(m_worker, "requestRender", Qt::QueuedConnection);
}
