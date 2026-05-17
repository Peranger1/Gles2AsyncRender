#pragma once

#include "framework/execution/execution_common.h"
#include "photo_editor_render_args.h"
#include "photo_editor_result_types.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>

class PhotoEditorGpuSession final : public QObject
{
    Q_OBJECT

public:
    explicit PhotoEditorGpuSession(QObject *parent = nullptr);
    ~PhotoEditorGpuSession() override;

    ExecutionOutcome<void> initialize(IRuntime *runtime);
    void shutdown(IRuntime *runtime);

    ExecutionOutcome<void> ensureSource(IRuntime *runtime,
                                        const PhotoEditorSourceSnapshot &source,
                                        const QSize &outputSize);

    void renderPreviewAsync(const TaskContext &context,
                            const PhotoEditorGpuPreviewArgs &args,
                            std::function<void(ExecutionOutcome<RawGpuTextureResult>)> done);

private slots:
    void onProgressEvent(int progress, bool isEnd);

private:
    static void processProgressThunk(int progress, bool isEnd, void *userData);

    ExecutionOutcome<void> ensureLibraryInitialized(IRuntime *runtime);
    ExecutionOutcome<void> rebuildSession(IRuntime *runtime,
                                          const PhotoEditorSourceSnapshot &source,
                                          const QSize &outputSize);
    ExecutionOutcome<RawGpuTextureResult> collectRenderResult(IRuntime *runtime);
    void destroySession(IRuntime *runtime);

    IRuntime *m_runtime = nullptr;
    void *m_handle = nullptr;
    bool m_libraryInitialized = false;

    QString m_sourceKey;
    quint64 m_sourceImageCacheKey = 0;
    QImage m_sourceImage;

    TaskId m_activeTaskId = 0;
    QString m_activeError;
    std::function<void(ExecutionOutcome<RawGpuTextureResult>)> m_activeDone;
};
