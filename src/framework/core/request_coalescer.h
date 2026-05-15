#pragma once

#include "work_types.h"

#include <QString>

enum class MergeDisposition
{
    KeepExistingPending,
    ReplacePending,
    MergeIntoPending,
    RejectMerge
};

class IRequestCoalescer
{
public:
    virtual ~IRequestCoalescer() = default;

    virtual bool canCoalesce(const WorkEnvelope &older,
                             const WorkEnvelope &newer) const = 0;

    virtual MergeDisposition merge(const WorkEnvelope &olderPending,
                                   const WorkEnvelope &newerIncoming,
                                   WorkEnvelope *mergedPending,
                                   QString *error) const = 0;
};
