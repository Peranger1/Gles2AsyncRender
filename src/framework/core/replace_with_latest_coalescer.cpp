#include "replace_with_latest_coalescer.h"

namespace
{
bool sameLane(const WorkEnvelope &older, const WorkEnvelope &newer)
{
    return older.laneId == newer.laneId;
}

bool sameMergeKey(const WorkEnvelope &older, const WorkEnvelope &newer)
{
    return !older.mergeKey.isEmpty()
        && !newer.mergeKey.isEmpty()
        && older.mergeKey == newer.mergeKey;
}
}

bool ReplaceWithLatestCoalescer::canCoalesce(const WorkEnvelope &older,
                                             const WorkEnvelope &newer) const
{
    return sameLane(older, newer) && sameMergeKey(older, newer);
}

MergeDisposition ReplaceWithLatestCoalescer::merge(const WorkEnvelope &olderPending,
                                                   const WorkEnvelope &newerIncoming,
                                                   WorkEnvelope *mergedPending,
                                                   QString *error) const
{
    Q_UNUSED(error);

    if (!mergedPending) {
        return MergeDisposition::RejectMerge;
    }

    if (!canCoalesce(olderPending, newerIncoming)) {
        *mergedPending = newerIncoming;
        return MergeDisposition::ReplacePending;
    }

    *mergedPending = newerIncoming;
    return MergeDisposition::ReplacePending;
}
