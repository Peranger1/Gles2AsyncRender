#pragma once

#include "photo_editor_render_payload.h"

#include <QObject>
#include <QImage>
#include <QString>
#include <QtANGLE/GLES2/gl2.h>

#include <memory>

class AngleStandaloneRuntime;

using PhotoEditorProgressCallback = void (*)(int progress, bool isEnd, void *userData);

class PhotoEditorRenderSession final
{
public:
    bool initialize(AngleStandaloneRuntime *runtime, QString *error);
    bool submitRequest(const std::shared_ptr<PhotoEditorRenderPayload> &payload,
                       QObject *callbackContext,
                       PhotoEditorProgressCallback progressCallback,
                       void *progressUserData,
                       QString *error);
    void handleProgressEvent(int progress, bool isEnd);
    bool renderReadyTexture(GLuint *textureId, QSize *size, QString *error);
    void shutdown();

private:
    struct SessionState final
    {
        void *handle = nullptr;
        ImageEffectParameters latestParameters;
        ImageEffectParameters processingParameters;
        bool hasLatestParameters = false;
        bool parametersDirty = false;
        int latestProgress = 0;
        bool processInFlight = false;
        bool renderReady = false;

        void reset();
    };

    static bool initializeLibraryOnce(AngleStandaloneRuntime *runtime, QString *error);
    static void *resolveGlProc(const char *name);

    bool ensureSessionForPayload(const QString &sourceKey,
                                 quint64 sourceImageCacheKey,
                                 const QImage &sourceImage,
                                 const QSize &outputSize,
                                 QString *error);
    bool rebuildSession(const QImage &sourceImage, const QSize &outputSize, QString *error);
    void destroySession();

    AngleStandaloneRuntime *m_runtime = nullptr;
    SessionState m_session;
    QString m_sourceKey;
    quint64 m_sourceImageCacheKey = 0;
    QImage m_sourceImage;
};
