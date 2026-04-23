#pragma once

#include <QRecursiveMutex>

QRecursiveMutex &sharedGlesMutex();

class ScopedGlesLock final
{
public:
    ScopedGlesLock();
    ~ScopedGlesLock();

    Q_DISABLE_COPY(ScopedGlesLock)

private:
    QRecursiveMutex &m_mutex;
};
