#pragma once

#include <stdexcept>
#include <string>

namespace async
{
class FutureException : public std::logic_error
{
public:
    explicit FutureException(const char *message)
        : std::logic_error(message)
    {
    }

    explicit FutureException(const std::string &message)
        : std::logic_error(message)
    {
    }
};

class FutureInvalid : public FutureException
{
public:
    FutureInvalid()
        : FutureException("Future invalid")
    {
    }
};

class PromiseInvalid : public FutureException
{
public:
    PromiseInvalid()
        : FutureException("Promise invalid")
    {
    }
};

class FutureAlreadyRetrieved : public FutureException
{
public:
    FutureAlreadyRetrieved()
        : FutureException("Future already retrieved")
    {
    }
};

class PromiseAlreadySatisfied : public FutureException
{
public:
    PromiseAlreadySatisfied()
        : FutureException("Promise already satisfied")
    {
    }
};

class BrokenPromise : public FutureException
{
public:
    BrokenPromise()
        : FutureException("Broken promise")
    {
    }
};

class FutureCancelled : public FutureException
{
public:
    FutureCancelled()
        : FutureException("Future cancelled")
    {
    }
};

class FutureTimeout : public FutureException
{
public:
    FutureTimeout()
        : FutureException("Future timed out")
    {
    }
};
}
