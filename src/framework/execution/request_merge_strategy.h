#pragma once

#include "execution_types.h"

#include <QString>

enum class MergeDisposition
{
    KeepExistingWaiting,
    ReplaceWaiting,
    Reject
};

class IRequestMergeStrategy
{
public:
    virtual ~IRequestMergeStrategy() = default;

    virtual bool canMerge(const ExecutionRequest &older, const ExecutionRequest &newer) const = 0;
    virtual MergeDisposition merge(const ExecutionRequest &olderWaiting,
                                   const ExecutionRequest &newerIncoming,
                                   ExecutionRequest *mergedWaiting,
                                   QString *error) const = 0;
};

class ReplaceWithLatestMergeStrategy final : public IRequestMergeStrategy
{
public:
    bool canMerge(const ExecutionRequest &older, const ExecutionRequest &newer) const override
    {
        return !older.mergeKey.isEmpty()
            && !newer.mergeKey.isEmpty()
            && older.mergeKey == newer.mergeKey
            && older.typeId == newer.typeId;
    }

    MergeDisposition merge(const ExecutionRequest &olderWaiting,
                           const ExecutionRequest &newerIncoming,
                           ExecutionRequest *mergedWaiting,
                           QString *error) const override
    {
        Q_UNUSED(error);

        if (!mergedWaiting) {
            return MergeDisposition::Reject;
        }

        if (!canMerge(olderWaiting, newerIncoming)) {
            return MergeDisposition::Reject;
        }

        *mergedWaiting = newerIncoming;
        return MergeDisposition::ReplaceWaiting;
    }
};
