#pragma once

#include <QSize>

class IPresentationTarget
{
public:
    virtual ~IPresentationTarget() = default;

    virtual QSize targetSize() const = 0;
    virtual void requestPresent() = 0;
};
