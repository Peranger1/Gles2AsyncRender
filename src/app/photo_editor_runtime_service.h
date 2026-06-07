#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>

#include <memory>

class PhotoEditorHandleActor;

namespace execution
{
class RuntimeExecutor;
}

class PhotoEditorRuntimeService final : public QObject
{
    Q_OBJECT

public:
    explicit PhotoEditorRuntimeService(execution::RuntimeExecutor *executor, QObject *parent = nullptr);
    ~PhotoEditorRuntimeService() override;

    void initialize();
    std::shared_ptr<PhotoEditorHandleActor> createActor(QString sourceKey,
                                                        quint64 sourceImageCacheKey,
                                                        std::shared_ptr<const QImage> sourceImage);
    void shutdown();

signals:
    void warning(const QString &message);

private:
    void emitWarning(const QString &message);

    execution::RuntimeExecutor *m_executor = nullptr;
    mutable QMutex m_mutex;
    QHash<quint64, std::shared_ptr<PhotoEditorHandleActor>> m_actors;
    quint64 m_nextActorId = 0;
    bool m_initializePosted = false;
    bool m_initialized = false;
    bool m_shuttingDown = false;
};
