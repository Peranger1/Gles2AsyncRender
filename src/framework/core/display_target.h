#pragma once

#include <QSize>

class IDisplayTarget
{
public:
    virtual ~IDisplayTarget() = default;

    virtual QSize targetSize() const = 0;
    virtual void requestPresent() = 0;
};
