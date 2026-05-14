#pragma once

#include <QImage>
#include <QMap>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include <QtANGLE/GLES2/gl2.h>

#include <memory>

enum class WorkPriority
{
    Low,
    Normal,
    High
};

enum class CoalescingPolicy
{
    KeepAll,
    ReplaceByKey,
    DropIfBusy,
    LatestOnly
};

enum class WorkState
{
    Queued,
    Admitted,
    Executing,
    ProducingArtifact,
    Publishing,
    Published,
    Cancelled,
    Failed
};

struct WorkEnvelope final
{
    quint64 workId = 0;
    QString streamKey;
    QString workflowKey;
    WorkPriority priority = WorkPriority::Normal;
    CoalescingPolicy coalescing = CoalescingPolicy::ReplaceByKey;
    QMap<QString, QVariant> hints;
    std::shared_ptr<void> payload;
};

struct ArtifactDescriptor final
{
    quint64 artifactId = 0;
    QString artifactKey;
    QString kind;
    QSize logicalSize;
    QMap<QString, QVariant> metadata;
};

struct PublicationTicket final
{
    quint64 publicationId = 0;
    quint64 workId = 0;
    QString streamKey;
    ArtifactDescriptor artifact;
    QMap<QString, QVariant> transportMetadata;
};

struct TextureArtifact final
{
    GLuint textureId = 0U;
    QSize size;
    QMap<QString, QVariant> metadata;
};

struct ArtifactSnapshot final
{
    ArtifactDescriptor descriptor;
    GLuint textureId = 0U;
    quintptr sharedHandle = 0U;
    QImage cpuBitmap;
    std::shared_ptr<void> customObject;
};

struct PublishedFrame final
{
    int slotIndex = -1;
    quintptr sharedHandle = 0;
    QSize size;
    quint64 generation = 0;
    quint64 frameIndex = 0;
};
