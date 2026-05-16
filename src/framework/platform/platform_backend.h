#pragma once

#include <memory>

class IReader;
class IRuntime;
class IWriter;

class IPlatformBackend
{
public:
    virtual ~IPlatformBackend() = default;

    virtual std::unique_ptr<IRuntime> createRuntime() const = 0;
    virtual IReader *reader() const = 0;
    virtual IWriter *writer() const = 0;
};
