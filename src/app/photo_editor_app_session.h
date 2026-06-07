#pragma once

#include "image_effect_types.h"
#include "photo_editor/photo_editor_result_types.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

class IPlatformBackend;
class PhotoEditorHandleActor;
class PhotoEditorRuntimeService;

namespace execution
{
class RuntimeExecutor;
}

class PhotoEditorAppSession final : public QObject
{
    Q_OBJECT

public:
    explicit PhotoEditorAppSession(QObject *parent = nullptr);
    ~PhotoEditorAppSession() override;

public slots:
    bool initialize(IPlatformBackend *backend, QSize outputSize);
    void setOutputSize(QSize size, quint64 outputRevision);
    void setEffectParameters(const ImageEffectParameters &parameters);
    void loadImageDirectory(const QString &directoryPath);
    void selectNextImage();
    void selectPreviousImage();
    void requestRender();
    void requestCpuPreview();
    void shutdown();

signals:
    void initializationFailed(const QString &reason);
    void requestWarning(const QString &reason);
    void imageDirectoryLoadFinished(bool loaded,
                                    const QString &errorMessage,
                                    int currentIndex,
                                    int count,
                                    const QString &displayName,
                                    QSize imageSize);
    void imageSelectionChanged(int currentIndex, int count, const QString &displayName, QSize imageSize);
    void cpuPreviewReady(const QImage &image, const QString &description);

private:
    struct ImageCatalogState;

    void emitImageSelection();
    void submitGpuPreview();
    void runCpuPreviewSync();
    QString currentGpuActorKey() const;
    std::shared_ptr<PhotoEditorHandleActor> currentGpuActor();
    void handleGpuRenderResult(const RawGpuTextureResult &result);

    std::unique_ptr<ImageCatalogState> m_catalog;
    IPlatformBackend *m_backend = nullptr;
    std::unique_ptr<execution::RuntimeExecutor> m_runtimeExecutor;
    std::unique_ptr<PhotoEditorRuntimeService> m_runtimeService;
    QHash<QString, std::shared_ptr<PhotoEditorHandleActor>> m_gpuActors;
    std::shared_ptr<PhotoEditorHandleActor> m_currentGpuActor;
    ImageEffectParameters m_effectParameters;
    QSize m_outputSize;
    quint64 m_outputRevision = 0;
    bool m_initialized = false;
    bool m_shuttingDown = false;
};
