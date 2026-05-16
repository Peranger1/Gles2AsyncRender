#include "request_dispatcher.h"

void RequestDispatcher::registerChannel(std::unique_ptr<RequestChannel> channel)
{
    if (!channel) {
        return;
    }

    m_channels.push_back(std::move(channel));
}

void RequestDispatcher::submit(const ExecutionRequest &request)
{
    for (const auto &channel : m_channels) {
        if (channel && channel->typeId() == request.typeId) {
            channel->submit(request);
            return;
        }
    }
}

void RequestDispatcher::shutdown()
{
    for (const auto &channel : m_channels) {
        if (channel) {
            channel->shutdown();
        }
    }
    m_channels.clear();
}
