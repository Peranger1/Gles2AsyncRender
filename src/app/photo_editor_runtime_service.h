#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>

#include <memory>

class PhotoEditorHandleActor;
class RuntimeExecutor;
class RuntimeHost;

class PhotoEditorRuntimeService final : public QObject
{
    Q_OBJECT

public:
    explicit PhotoEditorRuntimeService(RuntimeHost *host, QObject *parent = nullptr);
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

    std::unique_ptr<RuntimeExecutor> m_executor;
    mutable QMutex m_mutex;
    QHash<quint64, std::shared_ptr<PhotoEditorHandleActor>> m_actors;
    quint64 m_nextActorId = 0;
    bool m_initializePosted = false;
    bool m_initialized = false;
    bool m_shuttingDown = false;
};
