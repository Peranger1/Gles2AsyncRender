#pragma once

#include "image_effect_types.h"
#include "photo_editor/photo_editor_result_types.h"

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

namespace async
{
struct Unit;
template <typename T>
class Future;
}

namespace execution
{
class RuntimeExecutor;
}

class PhotoEditorHandleActor final : public QObject, public std::enable_shared_from_this<PhotoEditorHandleActor>
{
    Q_OBJECT

public:
    enum class State
    {
        Empty,
        Creating,
        Ready,
        Processing,
        Processed,
        Rendering,
        Rendered,
        Destroying,
        Destroyed,
        Failed
    };

    PhotoEditorHandleActor(quint64 actorId,
                           execution::RuntimeExecutor *executor,
                           QString sourceKey,
                           quint64 sourceImageCacheKey,
                           std::shared_ptr<const QImage> sourceImage,
                           QObject *parent = nullptr);
    ~PhotoEditorHandleActor() override;

    quint64 actorId() const;
    QString sourceKey() const;
    quint64 sourceImageCacheKey() const;
    State state() const;

    void create();
    void setOutputSize(QSize size);
    void setOpcode(ImageEffectParameters parameters);
    void process();
    void render();
    void destroy();
    bool destroySync(QString *error = nullptr);

signals:
    void warning(const QString &message);
    void renderResult(const RawGpuTextureResult &result);

private:
    void postCreateTask(quint64 generation);
    void postSetOutputSizeTask(quint64 generation);
    void postSetOpcodeTask(quint64 generation);
    void postProcessTask(quint64 generation);
    void postRenderTask(quint64 generation);
    void postDestroyTask(void *handle, quint64 generation);
    void observeTask(async::Future<async::Unit> future,
                     quint64 generation,
                     QString fallback,
                     bool failCurrent);

    void onProcessCompleted(quint64 generation, int progress);
    void handlePhotoEditorProgress(int progress, bool isEnd);
    void failIfCurrent(quint64 generation, const QString &message);
    void warn(const QString &message);

    bool isCurrentLocked(quint64 generation) const;
    bool canUseHandleLocked() const;
    static void onPhotoEditorProgress(int progress, bool isEnd, void *userData);
    static QString stateName(State state);

    quint64 m_actorId = 0;
    execution::RuntimeExecutor *m_executor = nullptr;
    void *m_handle = nullptr;
    QString m_sourceKey;
    quint64 m_sourceImageCacheKey = 0;
    std::shared_ptr<const QImage> m_sourceImage;
    ImageEffectParameters m_latestParameters;
    QSize m_outputSize;
    quint64 m_generation = 0;
    State m_state = State::Empty;
    bool m_destroyRequested = false;
    bool m_processAgainRequested = false;
    bool m_hasOutputSize = false;
    bool m_hasOpcode = false;
    bool m_outputSizeTaskPending = false;
    bool m_opcodeTaskPending = false;
    bool m_processTaskPending = false;
    quint64 m_activeProcessGeneration = 0;
    mutable QMutex m_mutex;
};
