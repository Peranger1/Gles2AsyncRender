#include "async_render_main_window.h"

#include "app/photo_editor_async_render_facade.h"
#include "framework/qt/qopenglwidget_frame_view.h"
#include "framework/backend/win_angle_d3d11/d3d11_shared_slot_pool.h"
#include "framework/core/shared_frame_slot_pool.h"
#include "runtime_diagnostics.h"

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

namespace
{
void logWindowMessage(const QString &message)
{
    RuntimeDiagnostics::logInfo("[AsyncRenderMainWindow]", message);
}

void logWindowDiag(const QString &message)
{
    RuntimeDiagnostics::logDiag("[AsyncRenderMainWindow]", message);
}
}

AsyncRenderMainWindow::AsyncRenderMainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_displayWidget(new QOpenGLWidgetFrameView(this))
    , m_slotPool(std::make_shared<D3D11SharedSlotPool>(3))
    , m_worker(new PhotoEditorAsyncRenderFacade())
{
    setWindowTitle(QStringLiteral("Gles2AsyncRender"));
    resize(1280, 760);

    m_displayWidget->setSlotPool(m_slotPool);
    setCentralWidget(m_displayWidget);

    m_workerThread.setObjectName(QStringLiteral("PhotoEditorAsyncRenderFacadeThread"));
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_displayWidget, &QOpenGLWidgetFrameView::glInitialized,
            this, &AsyncRenderMainWindow::onDisplayGlInitialized,
            Qt::QueuedConnection);
    connect(m_displayWidget, &QOpenGLWidgetFrameView::displayReadyForWorker,
            this, &AsyncRenderMainWindow::onDisplayReadyForWorker,
            Qt::QueuedConnection);
    connect(m_displayWidget, &QOpenGLWidgetFrameView::publicationCapacityAvailable,
            m_worker, &PhotoEditorAsyncRenderFacade::onPublicationCapacityAvailable,
            Qt::QueuedConnection);
    connect(m_displayWidget, &QOpenGLWidgetFrameView::outputSizeChanged,
            m_worker, &PhotoEditorAsyncRenderFacade::setOutputSize,
            Qt::QueuedConnection);

    connect(m_worker, &PhotoEditorAsyncRenderFacade::frameReady,
            m_displayWidget, &QOpenGLWidgetFrameView::onFrameReady,
            Qt::QueuedConnection);
    connect(m_worker, &PhotoEditorAsyncRenderFacade::initializationFailed,
            this, &AsyncRenderMainWindow::onWorkerError,
            Qt::QueuedConnection);
    connect(m_worker, &PhotoEditorAsyncRenderFacade::imageDirectoryLoadFinished,
            this, &AsyncRenderMainWindow::onImageDirectoryLoadFinished,
            Qt::QueuedConnection);
    connect(m_worker, &PhotoEditorAsyncRenderFacade::imageSelectionChanged,
            this, &AsyncRenderMainWindow::onImageSelectionChanged,
            Qt::QueuedConnection);

    m_workerThread.start();

    setupActions();
    setupImageEffectControls();
    updateImageActions();
    updateStatusBarMessage();
}

AsyncRenderMainWindow::~AsyncRenderMainWindow()
{
    logWindowDiag(QStringLiteral("Destructor begin workerThreadRunning=%1 workerInitialized=%2")
                      .arg(m_workerThread.isRunning())
                      .arg(m_workerInitialized));
    if (m_workerThread.isRunning() && m_worker != nullptr) {
        logWindowDiag(QStringLiteral("Invoking worker shutdown."));
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait();
        logWindowDiag(QStringLiteral("Worker thread stopped."));
    }

    m_worker = nullptr;
    logWindowDiag(QStringLiteral("Destructor end"));
}

void AsyncRenderMainWindow::onDisplayGlInitialized()
{
    logWindowMessage(QStringLiteral("Display import widget is ready. Waiting for the first frame swap before starting the D3D11 worker."));
}

void AsyncRenderMainWindow::onDisplayReadyForWorker()
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
        Q_ARG(ISharedFrameSlotPool *, m_slotPool.get()),
        Q_ARG(QSize, m_displayWidget->outputPixelSize()));
    initialized = invoked && initialized;
    if (!initialized) {
        logWindowMessage(QStringLiteral("D3D11 native worker initialization failed."));
        return;
    }

    m_workerInitialized = true;
    pushEffectParameters();
    requestRender();
    updateImageActions();
    logWindowMessage(QStringLiteral("D3D11 native worker is ready. Import an image directory to start the demo."));
    updateStatusBarMessage();
}

void AsyncRenderMainWindow::openImageDirectory()
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
    logWindowMessage(QStringLiteral("Loading image directory into D3D11 worker: %1").arg(directoryPath));
}

void AsyncRenderMainWindow::showNextImage()
{
    if (!m_workerInitialized || m_imageCount <= 0) {
        return;
    }

    QMetaObject::invokeMethod(m_worker, "selectNextImage", Qt::QueuedConnection);
}

void AsyncRenderMainWindow::showPreviousImage()
{
    if (!m_workerInitialized || m_imageCount <= 0) {
        return;
    }

    QMetaObject::invokeMethod(m_worker, "selectPreviousImage", Qt::QueuedConnection);
}

void AsyncRenderMainWindow::onImageEffectControlChanged()
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

void AsyncRenderMainWindow::resetImageEffects()
{
    m_effectParameters = {};
    setImageEffectControlsFromState();
    pushEffectParameters();
    requestRender();
}

void AsyncRenderMainWindow::onImageDirectoryLoadFinished(bool loaded,
                                                         const QString &errorMessage,
                                                         int currentIndex,
                                                         int count,
                                                         const QString &displayName,
                                                         QSize imageSize)
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
        m_currentImageSize = QSize();
        updateImageActions();
        logWindowMessage(QStringLiteral("Image directory loading failed."));
        updateStatusBarMessage();
        return;
    }

    m_currentImageIndex = currentIndex;
    m_imageCount = count;
    m_currentImageName = displayName;
    m_currentImageSize = imageSize;
    updateImageActions();
    updateStatusBarMessage();
}

void AsyncRenderMainWindow::onImageSelectionChanged(int currentIndex, int count, const QString &displayName, QSize imageSize)
{
    m_currentImageIndex = currentIndex;
    m_imageCount = count;
    m_currentImageName = displayName;
    m_currentImageSize = imageSize;
    updateImageActions();
    updateStatusBarMessage();
}

void AsyncRenderMainWindow::onWorkerError(const QString &reason)
{
    logWindowMessage(QStringLiteral("Worker error: %1").arg(reason));
    QMessageBox::warning(this, QStringLiteral("Worker Error"), reason);
}

void AsyncRenderMainWindow::setupActions()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
    m_openDirectoryAction = fileMenu->addAction(QStringLiteral("Open Image Directory"));
    m_openDirectoryAction->setShortcut(QKeySequence::Open);
    connect(m_openDirectoryAction, &QAction::triggered, this, &AsyncRenderMainWindow::openImageDirectory);

    QMenu *imageMenu = menuBar()->addMenu(QStringLiteral("Image"));
    m_previousImageAction = imageMenu->addAction(QStringLiteral("Previous"));
    m_previousImageAction->setShortcut(QKeySequence::MoveToPreviousPage);
    connect(m_previousImageAction, &QAction::triggered, this, &AsyncRenderMainWindow::showPreviousImage);

    m_nextImageAction = imageMenu->addAction(QStringLiteral("Next"));
    m_nextImageAction->setShortcut(QKeySequence::MoveToNextPage);
    connect(m_nextImageAction, &QAction::triggered, this, &AsyncRenderMainWindow::showNextImage);

    imageMenu->addSeparator();
    QAction *resetEffectsAction = imageMenu->addAction(QStringLiteral("Reset Effects"));
    connect(resetEffectsAction, &QAction::triggered, this, &AsyncRenderMainWindow::resetImageEffects);

    QMenu *renderMenu = menuBar()->addMenu(QStringLiteral("Render"));
    QAction *requestFrameAction = renderMenu->addAction(QStringLiteral("Render Once"));
    connect(requestFrameAction, &QAction::triggered, this, &AsyncRenderMainWindow::requestRender);
}

void AsyncRenderMainWindow::setupImageEffectControls()
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
    layout->addWidget(new QLabel(QStringLiteral("GPU Stress Loops runs additional GLES2 simulator iterations before the result is published into the shared D3D11 slot."), panel));
    layout->addWidget(new QLabel(QStringLiteral("The worker now uses an independent standalone ANGLE runtime. Publish mode defaults to standalone GPU publish and falls back to CPU only if required."), panel));
    layout->addStretch(1);

    connect(m_brightnessSlider, &QSlider::valueChanged, this, &AsyncRenderMainWindow::onImageEffectControlChanged);
    connect(m_contrastSlider, &QSlider::valueChanged, this, &AsyncRenderMainWindow::onImageEffectControlChanged);
    connect(m_zoomSlider, &QSlider::valueChanged, this, &AsyncRenderMainWindow::onImageEffectControlChanged);
    connect(m_panXSlider, &QSlider::valueChanged, this, &AsyncRenderMainWindow::onImageEffectControlChanged);
    connect(m_panYSlider, &QSlider::valueChanged, this, &AsyncRenderMainWindow::onImageEffectControlChanged);
    connect(m_rotationSlider, &QSlider::valueChanged, this, &AsyncRenderMainWindow::onImageEffectControlChanged);
    connect(m_heavyGpuSlider, &QSlider::valueChanged, this, &AsyncRenderMainWindow::onImageEffectControlChanged);
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

void AsyncRenderMainWindow::setImageEffectControlsFromState()
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

void AsyncRenderMainWindow::updateStatusBarMessage()
{
    const QString indexText = QString::number(m_currentImageIndex);
    const QString countText = QString::number(m_imageCount);
    const QString nameText = m_currentImageName.isEmpty() ? QStringLiteral("-") : m_currentImageName;
    const QString sizeText = m_currentImageSize.isValid()
        ? QStringLiteral("%1x%2").arg(m_currentImageSize.width()).arg(m_currentImageSize.height())
        : QStringLiteral("-");
    statusBar()->showMessage(QStringLiteral("m_currentImageIndex=%1  m_imageCount=%2  m_currentImageName=%3  imageSize=%4")
                                 .arg(indexText, countText, nameText, sizeText));
}

void AsyncRenderMainWindow::updateImageActions()
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

void AsyncRenderMainWindow::pushEffectParameters()
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

void AsyncRenderMainWindow::requestRender()
{
    if (!m_workerInitialized || m_worker == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(m_worker, "requestRender", Qt::QueuedConnection);
}
