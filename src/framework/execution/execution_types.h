#pragma once

#include "framework/platform/runtime_types.h"

#include <QImage>
#include <QMap>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include <QtANGLE/GLES2/gl2.h>

#include <memory>
#include <variant>

using RequestId = quint64;

enum class DeviceKind
{
    Cpu,
    Gpu
};

enum class CompletionKind
{
    Sync,
    Async
};

enum class RequestQueuePolicyKind
{
    MergeWhileBusy,
    SerialQueue
};

class IRequestPayload
{
public:
    virtual ~IRequestPayload() = default;
};

class ICustomResult
{
public:
    virtual ~ICustomResult() = default;
};

struct RawGpuTextureResult final
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

struct RequestTypeDescriptor final
{
    QString typeId;
    DeviceKind device = DeviceKind::Cpu;
    CompletionKind completion = CompletionKind::Sync;
    RuntimeKind runtimeKind = RuntimeKind::None;
    RequestQueuePolicyKind queuePolicy = RequestQueuePolicyKind::SerialQueue;
};

struct ExecutionRequest final
{
    RequestId requestId = 0;
    QString typeId;
    QString mergeKey;
    std::shared_ptr<IRequestPayload> payload;
};

using ExecutionResultPayload = std::variant<RawGpuTextureResult,
                                            CpuImageResult,
                                            std::shared_ptr<ICustomResult>>;

struct ExecutionResult final
{
    RequestId requestId = 0;
    QString typeId;
    ExecutionResultPayload payload;
};
