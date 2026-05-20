#pragma once

#include <memory>
#include <utility>

class IPlatformBackend;

template <typename Backend, typename... BackendArgs>
inline std::unique_ptr<IPlatformBackend> createPlatformBackend(BackendArgs &&...args)
{
    return std::make_unique<Backend>(std::forward<BackendArgs>(args)...);
}

std::unique_ptr<IPlatformBackend> createDefaultPlatformBackend(int slotCount = 3);
