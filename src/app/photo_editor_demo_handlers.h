#pragma once

#include "framework/execution/request_handler.h"

#include <QObject>

class PhotoEditorGpuPreviewHandler final : public QObject, public IRequestHandler
{
    Q_OBJECT

public:
    explicit PhotoEditorGpuPreviewHandler(QObject *parent = nullptr);
    ~PhotoEditorGpuPreviewHandler() override;

    RequestTypeDescriptor descriptor() const override;
    StartDisposition start(const ExecutionRequest &request,
                           IExecutionContext &context,
                           std::unique_ptr<IRequestExecution> *asyncExecution,
                           ExecutionResult *inlineResult,
                           QString *error) override;
    void shutdown() override;

private slots:
    void onProgressEvent(int progress, bool isEnd);

private:
    static void processProgressThunk(int progress, bool isEnd, void *userData);

    bool ensureLibraryInitialized(QString *error);
    bool ensureSessionForPayload(const QString &sourceKey,
                                 quint64 sourceImageCacheKey,
                                 const QImage &sourceImage,
                                 const QSize &outputSize,
                                 QString *error);
    bool rebuildSession(const QImage &sourceImage, const QSize &outputSize, QString *error);
    void destroySession();
    void clearActiveExecution();

    IRuntime *m_runtime = nullptr;
    void *m_handle = nullptr;
    bool m_libraryInitialized = false;
    QString m_sourceKey;
    quint64 m_sourceImageCacheKey = 0;
    QImage m_sourceImage;
    RequestId m_activeRequestId = 0;
    QString m_activeTypeId;
    IRequestExecution *m_activeExecution = nullptr;
};

class PhotoEditorCpuPreviewHandler final : public IRequestHandler
{
public:
    RequestTypeDescriptor descriptor() const override;
    StartDisposition start(const ExecutionRequest &request,
                           IExecutionContext &context,
                           std::unique_ptr<IRequestExecution> *asyncExecution,
                           ExecutionResult *inlineResult,
                           QString *error) override;
    void shutdown() override;
};
