#pragma once

#include "photo_editor_library_host.h"
#include "photo_editor_session.h"

#include <QObject>
#include <QImage>
#include <QString>
#include <QtANGLE/GLES2/gl2.h>

#include <memory>

class AngleStandaloneRuntime;

using PhotoEditorProgressCallback = void (*)(int progress, bool isEnd, void *userData);

struct PhotoEditorRequest final
{
    quint64 sequence = 0;
    QSize outputSize;
    std::shared_ptr<void> payload;
};

struct PhotoEditorRenderedTexture final
{
    GLuint textureId = 0U;
    QSize size;
};

class PhotoEditorRenderSession final
{
public:
    bool initialize(AngleStandaloneRuntime *runtime, QString *error);
    bool submitRequest(const PhotoEditorRequest &request,
                       QObject *callbackContext,
                       PhotoEditorProgressCallback progressCallback,
                       void *progressUserData,
                       QString *error);
    void handleProgressEvent(int progress, bool isEnd);
    bool isProcessInFlight() const noexcept;
    bool hasRenderReady() const noexcept;
    int latestProgress() const noexcept;
    quint64 requestSequence() const noexcept;
    bool renderReadyTexture(PhotoEditorRenderedTexture *output, QString *error);
    void discardRenderReady();
    void shutdown();

private:
    bool ensureSessionForPayload(const QString &sourceKey,
                                 quint64 sourceImageCacheKey,
                                 const QImage &sourceImage,
                                 const QSize &outputSize,
                                 QString *error);
    bool rebuildSession(const QImage &sourceImage, const QSize &outputSize, QString *error);
    void destroySession();

    AngleStandaloneRuntime *m_runtime = nullptr;
    PhotoEditorLibraryHost m_libraryHost;
    PhotoEditorSession m_session;
    QString m_sourceKey;
    quint64 m_sourceImageCacheKey = 0;
    QImage m_sourceImage;
    quint64 m_requestSequence = 0;
};
