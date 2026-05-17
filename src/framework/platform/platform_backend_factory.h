#pragma once

#include <memory>

class IPlatformBackend;

std::unique_ptr<IPlatformBackend> createDefaultPlatformBackend(int slotCount = 3);
