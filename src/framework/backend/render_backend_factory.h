#pragma once

#include <memory>

class IPlatformRenderBackend;

std::unique_ptr<IPlatformRenderBackend> createDefaultRenderBackend();
