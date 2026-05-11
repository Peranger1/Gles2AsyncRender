#pragma once

#include "src/framework/core/async_render_session.h"
#include "src/photo_editor_library_host.h"
#include "src/photo_editor_session.h"

#include <QImage>
#include <QString>

class PhotoEditorRenderSession final : public IAsyncRenderSession
{
public:
    bool initialize(IRenderRuntime &runtime, QString *error) override;
    bool submitRequest(const AsyncRenderRequest &request,
                       QObject *callbackContext,
                       AsyncRenderProgressCallback progressCallback,
                       void *progressUserData,
                       QString *error) override;
    void handleProgressEvent(int progress, bool isEnd) override;
    bool isProcessInFlight() const noexcept override;
    bool hasRenderReady() const noexcept override;
    int latestProgress() const noexcept override;
    quint64 requestSequence() const noexcept override;
    bool renderReadyTexture(RenderedTexture *output, QString *error) override;
    void discardRenderReady() override;
    void shutdown() override;

private:
    bool ensureSessionForPayload(const QString &sourceKey,
                                 quint64 sourceImageCacheKey,
                                 const QImage &sourceImage,
                                 const QSize &outputSize,
                                 QString *error);
    bool rebuildSession(const QImage &sourceImage, const QSize &outputSize, QString *error);
    void destroySession();

    IRenderRuntime *m_runtime = nullptr;
    PhotoEditorLibraryHost m_libraryHost;
    PhotoEditorSession m_session;
    QString m_sourceKey;
    quint64 m_sourceImageCacheKey = 0;
    QImage m_sourceImage;
    quint64 m_requestSequence = 0;
};
