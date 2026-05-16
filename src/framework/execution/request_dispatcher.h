#pragma once

#include "request_channel.h"

#include <vector>

class RequestDispatcher final
{
public:
    void registerChannel(std::unique_ptr<RequestChannel> channel);
    void submit(const ExecutionRequest &request);
    void shutdown();

private:
    std::vector<std::unique_ptr<RequestChannel>> m_channels;
};
