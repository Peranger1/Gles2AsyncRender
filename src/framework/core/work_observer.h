#pragma once

#include "work_types.h"

class IWorkObserver
{
public:
    virtual ~IWorkObserver() = default;

    virtual void onStateChanged(RequestId requestId, WorkState state) = 0;
    virtual void onProgress(RequestId requestId, int progress, bool isFinal) = 0;
    virtual void onMessage(RequestId requestId, const QString &message) = 0;
};
