#pragma once

#include "work_runtime.h"
#include "work_types.h"

#include <QString>

class IArtifactPublisher
{
public:
    virtual ~IArtifactPublisher() = default;

    virtual bool initialize(IWorkRuntime &runtime, QString *error) = 0;
    virtual bool publish(const WorkEnvelope &work,
                         const ArtifactSnapshot &artifact,
                         PublicationTicket *ticket,
                         QString *error) = 0;
    virtual void shutdown() = 0;
};
