#pragma once

#include "work_types.h"

class IWorkObserver
{
public:
    virtual ~IWorkObserver() = default;

    virtual void onStateChanged(quint64 workId, WorkState state) = 0;
    virtual void onProgress(quint64 workId, int progress, bool isFinal) = 0;
    virtual void onMessage(quint64 workId, const QString &message) = 0;
};
