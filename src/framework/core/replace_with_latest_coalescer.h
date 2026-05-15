#pragma once

#include "request_coalescer.h"

class ReplaceWithLatestCoalescer final : public IRequestCoalescer
{
public:
    bool canCoalesce(const WorkEnvelope &older,
                     const WorkEnvelope &newer) const override;

    MergeDisposition merge(const WorkEnvelope &olderPending,
                           const WorkEnvelope &newerIncoming,
                           WorkEnvelope *mergedPending,
                           QString *error) const override;
};
