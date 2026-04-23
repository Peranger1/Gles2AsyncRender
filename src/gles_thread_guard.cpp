#include "gles_thread_guard.h"

QRecursiveMutex &sharedGlesMutex()
{
    static QRecursiveMutex mutex;
    return mutex;
}

ScopedGlesLock::ScopedGlesLock()
    : m_mutex(sharedGlesMutex())
{
    m_mutex.lock();
}

ScopedGlesLock::~ScopedGlesLock()
{
    m_mutex.unlock();
}
