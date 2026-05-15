#pragma once

#include <QImage>
#include <QMap>
#include <QMetaType>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include <QtANGLE/GLES2/gl2.h>

#include <memory>
#include <variant>

using RequestId = quint64;
using RequestVersion = quint64;
using LaneId = QString;
using MergeKey = QString;

enum class RequestPriority
{
    Low,
    Normal,
    High
};

enum class ResultDeliveryPolicy
{
    AlwaysDeliver,
    DeliverOnlyIfLatest,
    DeliverOnlyIfNoPending
};

enum class WorkState
{
    Queued,
    Admitted,
    Executing,
    ProducingOutput,
    Publishing,
    Published,
    Cancelled,
    Failed
};

class IWorkPayload
{
public:
    virtual ~IWorkPayload() = default;
};

class ICustomResult
{
public:
    virtual ~ICustomResult() = default;
};

struct RequestHints final
{
    RequestPriority priority = RequestPriority::Normal;
    ResultDeliveryPolicy deliveryPolicy = ResultDeliveryPolicy::DeliverOnlyIfLatest;
};

struct RequestEnvelope final
{
    RequestId requestId = 0;
    RequestVersion version = 0;
    LaneId laneId;
    MergeKey mergeKey;
    QString requestKind;
    RequestHints hints;
    std::shared_ptr<IWorkPayload> payload;
};

using WorkEnvelope = RequestEnvelope;

struct LaneSnapshot final
{
    bool hasActive = false;
    bool hasPending = false;
    bool hasPublicationPending = false;
    bool isPublishing = false;
    WorkEnvelope active;
    WorkEnvelope pending;
    WorkEnvelope publication;
};

struct FrameTicket final
{
    int slotIndex = -1;
    quint64 generation = 0;
    quint64 frameIndex = 0;
    QSize size;
};

struct GpuTextureResult final
{
    GLuint textureId = 0U;
    QSize size;
    QMap<QString, QVariant> metadata;
};

struct CpuImageResult final
{
    QImage image;
    QMap<QString, QVariant> metadata;
};

using ProcessorOutputPayload = std::variant<GpuTextureResult,
                                            CpuImageResult,
                                            std::shared_ptr<ICustomResult>>;

struct ProcessorOutput final
{
    RequestId requestId = 0;
    QString outputKind;
    ProcessorOutputPayload payload;
};

using JobResultPayload = std::variant<FrameTicket,
                                      CpuImageResult,
                                      std::shared_ptr<ICustomResult>>;

struct JobResult final
{
    RequestId requestId = 0;
    QString resultKind;
    JobResultPayload payload;
};

struct FrameSlotInfo final
{
    quintptr sharedHandle = 0U;
    QSize size;
    quint64 generation = 0;
};

Q_DECLARE_METATYPE(FrameTicket)
Q_DECLARE_METATYPE(JobResult)
Q_DECLARE_METATYPE(WorkState)
